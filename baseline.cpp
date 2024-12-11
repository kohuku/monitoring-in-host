/*
 * Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */
#include <inttypes.h>
#include <map>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

//c++
#include <vector>
#include <memory>
#include <fstream>
#include <set>
#include <vector>
#include <iostream>
#include <time.h>
#include <dirent.h>
#include <sstream>  // for std::istringstream
#include <algorithm>  // std::all_of
#include <chrono>  // 高精度タイマー用
#include <thread>  // sleep用




// non-blocking comm
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 1024
#define PORT 12345
#define PROCESS_PID 0
#define PROCESS_LINUX_UID 1
#define PROCESS_PPID 2

std::map<int, int> pid_jobid;

/*
 * Calls the DOCA APSH API function that matches this sample name and prints the result
 *
 * @dma_device_name [in]: IBDEV Name of the device to use for DMA
 * @pci_vuid [in]: VUID of the device exposed to the target system
 * @os_type [in]: Indicates the OS type of the target system
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */

struct Node {
	unsigned int pid;
	unsigned int euid;
	std::set<unsigned int> child_pids;
	unsigned int ppid;
};

struct Process {
    unsigned int pid;
    unsigned int euid;
    unsigned int ppid;
};
//std::map<unsigned int, struct Node> tree; // {pid:node}

void printTree(std::map<unsigned int, struct Node>& tree) {
	// 出力ファイルを開く
    std::ofstream file("tree_output.txt");
    if (!file.is_open()) {
		printf("File couldn't open");
    }
	for(const auto &pair : tree){
		unsigned int pid = pair.first;
		file << "PID: " << pid << ", EUID: " << tree[pid].euid << ", PPID: " << tree[pid].ppid << std::endl;
	}
	file << std::endl;
	file.close();
}

int sig_stop_process(unsigned int pid){
	printf("priviledge escalation detected in pid %d\n",pid);
	printf("stopping JOB %d\n",pid_jobid[pid]);
	std::string job_id_str = std::to_string(pid_jobid[pid]);
	

	pid_t fork_pid = fork();

    if (fork_pid == -1) {
        perror("fork failed");
        return 1;
    }

    if (fork_pid == 0) {
        // 子プロセスで execlp を実行
        execlp("scancel", "scancel", job_id_str.c_str(), NULL);
        // execlp が失敗した場合のみここに到達
        perror("execlp failed");
        exit(1);
    } else {
        // 親プロセスで元の動作を続ける
        printf("Parent process: waiting for child to finish...\n");
        wait(NULL); // 子プロセスが終了するのを待つ
        printf("Child process finished. Parent continues.\n");
    }
	return 0;
}

int validate(unsigned int current_pid, unsigned int expected_euid, std::map<unsigned int, struct Node>& tree, unsigned int stepd_pid){
	int detected_count = 0;
	printf("VALIDATION: PID: %u, EUID: %u",current_pid,expected_euid);
	for(const unsigned int& next_pid : tree[current_pid].child_pids){
		detected_count = validate(next_pid, expected_euid, tree, stepd_pid);
	}
	if(tree[current_pid].euid != expected_euid){
		sig_stop_process(stepd_pid);
		return detected_count + 1;
	}
	return detected_count;
}


int create_server_fd(){
	int server_fd;
	int opt = 1;
	struct sockaddr_in address;
	// ソケットの作成
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        printf("socket failed");
        exit(EXIT_FAILURE);
    }

    // ソケットオプションの設定
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        printf("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // アドレスの設定
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // すべてのインターフェースで受け入れ
    address.sin_port = htons(PORT);

    // ソケットにアドレスをバインド
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 接続の待ち受け
    if (listen(server_fd, 3) < 0) {
        printf("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // ソケットをノンブロッキングに設定
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

	return server_fd;
}

// SPANK受け取ったstepdのデータ(data)とその数(num_data)
void receive_data_from_SPANK(int server_fd, unsigned int **data, unsigned int *num_data){
	int new_socket, valread, max_sd;
    struct sockaddr_in address;
    char *token;
    int addrlen = sizeof(address);
	char buffer[BUFFER_SIZE] = {0};
    struct timeval timeout;  // タイムアウト用の構造体

    fd_set readfds;
	// =========== while ===============
    // fd_set を初期化
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    max_sd = server_fd;
    // タイムアウトをゼロに設定（ノンブロッキングで即座に戻る）
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    // select() を使用してソケットを監視
    int activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);
    if ((activity < 0) && (errno != EINTR)) {
        printf("select error");
    }
    // 新しい接続がある場合
    if (FD_ISSET(server_fd, &readfds)) {
		// アドレスの設定
    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;  // すべてのインターフェースで受け入れ
    	address.sin_port = htons(PORT);
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (new_socket < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                printf("accept failed");
                close(server_fd);
                exit(EXIT_FAILURE);
            }
        } else {
            // 受信バッファをクリア
            memset(buffer, 0, BUFFER_SIZE);
            valread = read(new_socket, buffer, BUFFER_SIZE);
            if (valread > 0) {
                // 受信したデータを解析して動的に格納
                unsigned int *received_data = NULL;
                unsigned int count = 0;

                token = strtok(buffer, " ");
                while (token != NULL) {
                    count++;
                    received_data = static_cast<unsigned int*>(realloc(received_data, count * sizeof(unsigned int)));
                    if (received_data == NULL) {
                        printf("realloc failed");
                        close(new_socket);
                        return;
                    }
                    received_data[count - 1] = (unsigned int)atoi(token);
                    token = strtok(NULL, " ");
                }

                // 受信したデータとデータ数を設定
                *data = received_data;
                *num_data = count;
                for (unsigned int i = 0; i < count; i++) {
                    printf("%u ", received_data[i]);
                }
                free(received_data);
            }
            close(new_socket);  // ソケットを閉じて次の接続を待つ
        }
    }
}

// データ数が1なら削除 3なら追加
void update_stepd(std::map<unsigned int,unsigned int>& stepd, unsigned int *data, unsigned int num_data){
	if(num_data == 1){
		stepd.erase(data[0]);
		pid_jobid.erase(data[2]);
	}
	else {
		stepd[data[0]] = data[1];
		pid_jobid[data[0]] = data[2];
	}
}


// processes_get関数の定義
int processes_get(std::vector<Process>& pslist) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir failed");
        return -1; // エラーコードを返す
    }

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != nullptr) {
        // エントリがディレクトリであり、名前が完全に数値であることを確認
        std::string dir_name = entry->d_name;
        if (entry->d_type == DT_DIR && std::all_of(dir_name.begin(), dir_name.end(), ::isdigit)) {
            unsigned int pid = std::stoi(dir_name);
            std::string status_path = "/proc/" + dir_name + "/status";

            // 初期化
            unsigned int ppid = 0;
            unsigned int euid = 0;

            // /proc/<pid>/status を読み取る
            std::ifstream status_file(status_path);
            if (status_file.is_open()) {
                std::string line;
                while (std::getline(status_file, line)) {
                    // Pid: フィールド
                    if (line.find("Pid:") == 0) {
                        std::istringstream iss(line.substr(4));
                        iss >> pid;
                    }
                    // PPid: フィールド
                    else if (line.find("PPid:") == 0) {
                        std::istringstream iss(line.substr(5));
                        iss >> ppid;
                    }
                    // Uid: フィールド
                    else if (line.find("Uid:") == 0) {
                        std::istringstream iss(line.substr(4));
                        int real_uid, effective_uid, saved_uid, fs_uid;
                        if (iss >> real_uid >> effective_uid >> saved_uid >> fs_uid) {
                            euid = effective_uid;
                        }
                    }
                }
                status_file.close();

                // プロセス情報をリストに追加
                pslist.push_back({pid, euid, ppid});
            }
        }
    }

    closedir(proc_dir);
    return 0; // 成功を示す
}


int main()
{
	int num_processes, i;
	int server_fd = create_server_fd();
    std::vector<struct Process> pslist;
	std::map<unsigned int,unsigned int> euid_of_stepd; // {stepd pid: euid}
	std::map<unsigned int, struct Node> tree; // {pid:node}
	unsigned int pid = 0;
	unsigned int euid = 0;
	unsigned int ppid = 0;
	int flag = 0;

    #ifdef DEBUG
    auto start = std::chrono::high_resolution_clock::now(); // 開始時刻の記録
    #endif


	while(1){

		// get stepd info and create tree for each
		unsigned int *data = NULL;
        unsigned int num_data = 0;
        int result;
        receive_data_from_SPANK(server_fd,&data,&num_data);
		if(num_data) update_stepd(euid_of_stepd,data,num_data);
		
		
		result = processes_get(pslist);
        if (result) {
            	printf("Failed to create the process list");
                return result;
	    }
        num_processes = pslist.size();
        // create tree
		tree.clear();
		for (i = 0; i < num_processes; ++i) {
			pid = pslist[i].pid;
			euid = pslist[i].euid;
			ppid = pslist[i].ppid;

			if (ppid < 3)continue;
			
			struct Node node;
			node.pid = pid;
			node.ppid = ppid;
			node.euid = euid;
			tree[pid] = node;
			tree[ppid].child_pids.insert(pid);
		}

		
		//printTree(tree);

		// test for each stepd
        int is_detected = 0;
		for(std::map<unsigned int, unsigned int>::iterator it = euid_of_stepd.begin(); it != euid_of_stepd.end(); ++it){
			pid = it->first;
			euid = it->second;
			printf("%d %d",pid,euid);
			tree[pid].euid = euid;
			is_detected = validate(pid,euid,tree,pid);
		}

		pslist.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(37));

		#ifdef DEBUG
        auto end = std::chrono::high_resolution_clock::now();   // 終了時刻の記録
        std::chrono::duration<double> elapsed = end - start;    // 経過時間を計算
        std::cout << "Execution time: " << elapsed.count() << " seconds" << std::endl;
        #endif
	}

	return 0;
}