#include "server.h"
#include <signal.h>
#include <netinet/tcp.h> // TCP_NODELAY 옵션을 위해 포함

/**
 * @brief SIGINT(Ctrl+C) 시그널을 처리하는 핸들러.
 *        서버가 종료되기 전에 메모리에 있는 시청 기록을 파일에 안전하게 저장합니다.
 * @param sig 시그널 번호 (이 경우 SIGINT).
 */
void handle_sigint(int sig) {
    printf("\n[Signal] SIGINT received. Saving history and shutting down...\n");
    save_history_to_file(); 
    exit(0); 
}

/**
 * @brief OpenSSL 컨텍스트(SSL_CTX)를 생성하고 초기화합니다.
 *        SSL/TLS 통신의 기본 설정을 담당합니다.
 * @return 생성된 SSL_CTX 객체에 대한 포인터. 실패 시 프로그램을 종료합니다.
 */
SSL_CTX* create_context() {
    const SSL_METHOD *method = TLS_server_method(); // 최신 TLS 프로토콜을 사용하는 서버 메서드 선택
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

/**
 * @brief 생성된 SSL 컨텍스트에 서버 인증서와 개인키를 로드합니다.
 *        HTTPS 통신을 위해 필수적인 과정입니다.
 * @param ctx 설정할 SSL_CTX 객체.
 */
void configure_context(SSL_CTX *ctx) {
    // 서버의 공개 인증서(cert.pem) 로드
    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        perror("Error loading cert.pem");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    // 서버의 개인키(key.pem) 로드
    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0) {
        perror("Error loading key.pem");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief VIDEO_DIR에 있는 .mp4 파일들을 확인하여 썸네일이 없으면 생성합니다.
 *        서버 시작 시 한 번만 실행되며, `ffmpeg` 명령어를 사용합니다.
 */
void generate_thumbnails() {
    DIR *d = opendir(VIDEO_DIR);
    if (!d) {
        printf("[Error] Make sure '%s' exists.\n", VIDEO_DIR);
        return;
    }

    printf("[System] Checking videos in %s...\n", VIDEO_DIR);
    mkdir(THUMB_DIR, 0777); // 썸네일 디렉토리 생성 (없을 경우)

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        // .mp4 확장자를 가진 파일만 처리
        if (strstr(dir->d_name, ".mp4")) {
            char v_path[1024], t_path[1024], cmd[4096];
            
            // 비디오 파일과 생성될 썸네일 파일의 전체 경로 생성
            sprintf(v_path, "%s%s", VIDEO_DIR, dir->d_name);
            sprintf(t_path, "%s%s", THUMB_DIR, dir->d_name);
            char *dot = strrchr(t_path, '.'); 
            if(dot) strcpy(dot, ".jpg"); // 확장자를 .jpg로 변경

            // 썸네일 파일이 이미 존재하는지 확인
            if (access(t_path, F_OK) == -1) {
                // 썸네일이 없으면 ffmpeg을 통해 생성
                printf("[System] Generating thumbnail: %s\n", dir->d_name);
                sprintf(cmd, "ffmpeg -v quiet -i \"%s\" -ss 00:00:01 -vframes 1 -vf scale=320:-1 \"%s\"", v_path, t_path);
                system(cmd); // 외부 명령어 실행
            }
        }
    }
    closedir(d);
}

/**
 * @brief 서버의 메인 함수.
 *        프로그램의 시작점이며, 서버 초기화 및 클라이언트 연결 처리를 담당합니다.
 */
int main() {
    // --- 1. 시그널 핸들러 등록 ---
    signal(SIGINT, handle_sigint);  // Ctrl+C (SIGINT) 시그널 처리
    signal(SIGPIPE, SIG_IGN);       // 클라이언트가 갑자기 연결을 끊었을 때 발생하는 SIGPIPE 시그널을 무시 (서버 다운 방지)

    // --- 2. SSL/TLS 설정 ---
    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    // --- 3. 데이터 로딩 및 초기화 ---
    load_history();         // 파일에서 이전 시청 기록을 로드
    generate_thumbnails();  // 썸네일 자동 생성

    // --- 4. 소켓 설정 및 리스닝 ---
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    // 서버 소켓 생성
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed"); exit(EXIT_FAILURE);
    }
    
    // SO_REUSEADDR 옵션 설정: 서버가 비정상 종료 후 재시작 시 주소를 바로 재사용 가능하게 함
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Setsockopt failed"); exit(EXIT_FAILURE);
    }

    // 서버 주소 설정 (모든 IP에서 PORT로 들어오는 연결을 받음)
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 소켓에 주소 바인딩
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed"); exit(EXIT_FAILURE);
    }

    // 클라이언트 연결 대기열(backlog)을 10으로 설정하고 리스닝 시작
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed"); exit(EXIT_FAILURE);
    }

    printf("\n>>> FINAL HTTPS OTT Server Running on https://localhost:%d <<<\n", PORT);

    // --- 5. 클라이언트 연결 수락 및 스레드 생성 (무한 루프) ---
    while (1) {
        // 클라이언트 연결 수락 (blocking 함수)
        if ((client_fd = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            perror("Accept failed"); 
            continue; // 다음 연결을 기다림
        }

        // --- 소켓 옵션 설정 (스트리밍 성능 향상) ---
        int flag = 1;
        // TCP_NODELAY (네이글 알고리즘 비활성화): 작은 패킷을 즉시 보내 지연시간 감소
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
        // SO_SNDBUF (전송 버퍼 크기 증가): 대용량 데이터 전송 효율 향상
        int sndbuf = 1024 * 1024; // 1MB
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // 클라이언트 정보를 담을 구조체 동적 할당
        struct client_info *client = malloc(sizeof(struct client_info));
        client->socket = client_fd;
        client->address = address;
        client->ctx = ctx;

        // 클라이언트 처리를 위한 새 스레드 생성
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void*)client) < 0) {
            perror("Thread failed");
            // 스레드 생성 실패 시, 할당된 자원 해제
            free(client); 
            close(client_fd);
        } else {
            // 스레드가 종료될 때 자동으로 자원을 해제하도록 detach 상태로 만듦
            pthread_detach(thread_id);
        }
    }
    
    // 서버 종료 시 SSL 컨텍스트 해제
    SSL_CTX_free(ctx);
    return 0;
}