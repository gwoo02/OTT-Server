#ifndef SERVER_H
#define SERVER_H


#define _GNU_SOURCE          // 비표준 함수(예: strcasestr) 사용을 위해 정의
#include <stdio.h>           // 표준 입출력 (printf, fopen 등)
#include <stdlib.h>          // 메모리 할당 (malloc, free), 문자열 변환 (atoi)
#include <string.h>          // 문자열 처리 (strcpy, strcmp, strlen 등)
#include <unistd.h>          // UNIX 표준 함수 (read, write, close, access)
#include <arpa/inet.h>       // 인터넷 주소 변환 (inet_ntop 등)
#include <sys/socket.h>      // 소켓 프로그래밍 (socket, bind, listen, accept)
#include <sys/stat.h>        // 파일 상태 정보 (mkdir, fstat)
#include <sys/types.h>       // 기본 시스템 데이터 타입
#include <pthread.h>         // POSIX 스레드 (pthread_create, pthread_mutex)
#include <dirent.h>          // 디렉토리 관리 (opendir, readdir, closedir)
#include <ctype.h>           // 문자 분류 (isxdigit)
#include <fcntl.h>           // 파일 제어 (open 등)
#include <signal.h>          // 시그널 처리 (signal)
#include <sys/select.h>      // I/O 멀티플렉싱 (select)
#include <time.h>            // 시간 관련 함수 (time, strftime)

// HTTPS 통신을 위한 OpenSSL 라이브러리 헤더
#include <openssl/ssl.h>     // SSL/TLS 핵심 기능
#include <openssl/err.h>     // OpenSSL 오류 처리

// --- 서버 전역 설정 ---

// 서버가 리스닝할 포트 번호
#define PORT 8443 

// 비디오, 썸네일, 데이터 파일의 경로 정의
#define VIDEO_DIR  "./public/videos/"      // 비디오 파일 저장 디렉토리
#define THUMB_DIR  "./public/thumbnails/"  // 썸네일 이미지 저장 디렉토리
#define USER_DB    "./data/user.txt"       // 사용자 정보 텍스트 파일
#define HISTORY_DB "./data/history.dat"    // 시청 기록 바이너리 파일

// 파일 전송 시 사용할 버퍼의 크기 (512KB)
#define BSIZE      1024 * 512

// HSTS(HTTP Strict Transport Security) 헤더.
// 브라우저에게 항상 HTTPS로만 접속하라고 지시하여 프로토콜 다운그레이드 공격을 방지.
#define HSTS_HEADER "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n" 

// --- 구조체 정의 ---

/**
 * @brief 각 클라이언트의 정보를 담는 구조체.
 *        새로운 클라이언트 연결이 발생할 때마다 이 구조체가 생성되어 스레드로 전달됩니다.
 */
struct client_info {
    int socket;                  // 클라이언트와 연결된 소켓 파일 디스크립터
    struct sockaddr_in address;  // 클라이언트의 주소 정보 (IP, 포트)
    SSL_CTX *ctx;                // SSL 컨텍스트 (모든 스레드가 공유)
};


// --- 공통 함수 프로토타입 ---

/**
 * @brief 클라이언트의 요청을 처리하는 메인 핸들러 함수.
 *        각 클라이언트 연결에 대해 별도의 스레드에서 실행됩니다.
 * @param arg client_info 구조체에 대한 포인터.
 * @return NULL을 반환하고 스레드를 종료합니다.
 */
void *handle_client(void *arg);

/**
 * @brief 서버 시작 시 ./public/videos/ 디렉토리를 스캔하여
 *        썸네일이 없는 .mp4 파일에 대해 ffmpeg으로 썸네일을 자동 생성합니다.
 */
void generate_thumbnails();

/**
 * @brief 서버 시작 시 HISTORY_DB 파일에서 시청 기록을 읽어와 메모리에 로드합니다.
 */
void load_history();

/**
 * @brief 현재 메모리에 있는 시청 기록을 HISTORY_DB 파일에 저장합니다.
 *        주로 서버 종료 시 호출됩니다.
 */
void save_history_to_file();

#endif // SERVER_H