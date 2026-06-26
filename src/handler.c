#include "server.h"
#include <sys/select.h> 
#include <stdarg.h> // 가변 인자 처리를 위한 헤더

// --- 전역 데이터 구조 및 변수 ---

/**
 * @brief 사용자의 비디오 시청 기록을 저장하는 구조체.
 */
typedef struct {
    char username[50];     // 사용자 아이디
    char video_name[100];  // 비디오 파일 이름
    double last_time;      // 마지막으로 시청한 시간 (초)
    double duration;       // 비디오 총 길이 (초)
} WatchHistory;

// 전역 시청 기록 배열. 서버의 모든 사용자가 공유합니다.
// NOTE: 현재 구조는 서버 전체에 100개의 기록만 저장 가능하여 확장성에 한계가 있습니다.
static WatchHistory history[100] = {0}; 
static int history_count = 0; // 현재 저장된 시청 기록의 수

// 스레드 동기화를 위한 뮤텍스 (Mutex)
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER; // 시청 기록(history) 배열 접근 제어
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;    // 사용자 DB(user.txt) 파일 접근 제어
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;     // 로그 파일(server.log) 접근 제어


// --- 유틸리티 함수 ---

/**
 * @brief 현재 시간 타임스탬프를 "YYYY-MM-DD HH:MM:SS" 형식의 문자열로 반환합니다.
 * @return 정적 버퍼에 저장된 시간 문자열 포인터. (주의: 스레드 안전하지 않음)
 */
char* get_current_timestamp() {
    static char timestamp[30]; 
    time_t rawtime; struct tm *info;
    time(&rawtime); info = localtime(&rawtime);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", info);
    return timestamp;
}

/**
 * @brief 콘솔과 로그 파일에 동시에 로그 메시지를 기록하는 함수.
 *        가변 인자를 받아 printf와 유사하게 동작하며, 로그 파일 접근은 뮤텍스로 보호됩니다.
 * @param format 포맷 문자열.
 * @param ...    포맷에 대응하는 가변 인자.
 */
void log_message(const char *format, ...) {
    pthread_mutex_lock(&log_mutex); // 여러 스레드가 동시에 로그를 작성하여 내용이 꼬이는 것을 방지

    va_list args;

    // 1. 콘솔(터미널)에 로그 출력
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    // 2. 로그 파일(./data/server.log)에 로그 저장
    FILE *fp = fopen("./data/server.log", "a"); // 'a' (append) 모드로 열어 파일 끝에 추가
    if (fp) {
        va_start(args, format); // 가변 인자 목록 재설정
        vfprintf(fp, format, args);
        va_end(args);
        fclose(fp);
    } else {
        printf("[System Error] Cannot open ./data/server.log\n");
    }

    pthread_mutex_unlock(&log_mutex); // 잠금 해제
}

/**
 * @brief URL 인코딩된 문자열을 디코딩합니다.
 *        (예: "%20" -> " ", "+" -> " ")
 * @param dst 디코딩된 문자열이 저장될 버퍼.
 * @param src 원본 URL 인코딩된 문자열.
 */
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            // 16진수 문자를 값으로 변환
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = 16 * a + b; // 두 16진수 문자를 하나의 바이트로 조합
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' '; // '+' 문자를 공백으로 변환
            src++;
        } else {
            *dst++ = *src++; // 일반 문자는 그대로 복사
        }
    }
    *dst = '\0'; // 문자열 종료
}

/**
 * @brief 클라이언트에게 HTTP 오류 응답을 보냅니다.
 * @param ssl 클라이언트와 연결된 SSL 객체.
 * @param status 보낼 HTTP 상태 코드 (예: 404, 500).
 * @param title 오류 페이지의 제목.
 * @param text 오류 페이지에 표시할 설명 텍스트.
 */
void send_error(SSL *ssl, int status, const char* title, const char* text) {
    char body[8192];
    sprintf(body, "<html><head><meta charset='utf-8'><title>%d %s</title></head><body><h1>%d %s</h1><p>%s</p></body></html>", status, title, status, title, text);
    
    char header[512];
    sprintf(header, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", status, title, strlen(body));
    
    SSL_write(ssl, header, strlen(header)); 
    SSL_write(ssl, body, strlen(body));
}

// --- [시청 기록 관리 함수] ---

/**
 * @brief 서버 시작 시 `history.dat` 파일에서 시청 기록 데이터를 읽어 전역 `history` 배열로 로드합니다.
 */
void load_history() {
    FILE *fp = fopen(HISTORY_DB, "rb"); // 바이너리 읽기 모드로 파일 열기
    if (!fp) return;

    // 먼저 저장된 기록의 총 개수를 읽음
    fread(&history_count, sizeof(int), 1, fp);
    if (history_count > 0 && history_count <= 100) {
        // 유효한 개수만큼 history 구조체 배열을 통째로 읽어옴
        fread(&history, sizeof(WatchHistory), history_count, fp);
    } else {
        history_count = 0; // 파일이 손상되었거나 비어있으면 0으로 초기화
    }
    fclose(fp);
}

/**
 * @brief 메모리에 있는 현재 시청 기록(`history` 배열)을 `history.dat` 파일에 저장합니다.
 *        서버 종료와 같이 데이터 보존이 필요할 때 호출됩니다.
 */
void save_history_to_file() {
    pthread_mutex_lock(&history_mutex); // 파일에 쓰는 동안 다른 스레드의 접근을 막음
    
    FILE *fp = fopen(HISTORY_DB, "wb"); // 바이너리 쓰기 모드로 파일 열기 (기존 내용 덮어씀)
    if (fp) {
        fwrite(&history_count, sizeof(int), 1, fp); // 기록의 총 개수를 먼저 저장
        if (history_count > 0) {
            fwrite(&history, sizeof(WatchHistory), history_count, fp); // history 배열 전체를 파일에 씀
        }
        fclose(fp);
    }

    pthread_mutex_unlock(&history_mutex);
}

/**
 * @brief 특정 사용자의 모든 시청 기록을 메모리에서 삭제합니다.
 *        주로 회원 탈퇴와 같은 시나리오에서 사용될 수 있습니다.
 * @param user 기록을 삭제할 사용자 아이디.
 */
void remove_user_history(const char* user) {
    pthread_mutex_lock(&history_mutex);
    
    int i = 0;
    while (i < history_count) {
        if (strcmp(history[i].username, user) == 0) {
            // 해당 사용자의 기록을 찾으면, 그 위치부터 뒤의 모든 요소를 한 칸씩 앞으로 당김
            for (int j = i; j < history_count - 1; j++) {
                history[j] = history[j+1];
            }
            history_count--; // 전체 개수 감소
        } else {
            i++; // 해당 사용자 기록이 아니면 다음으로 이동
        }
    }

    pthread_mutex_unlock(&history_mutex);
    save_history_to_file(); // 변경사항을 즉시 파일에 저장
}

/**
 * @brief 사용자의 비디오 시청 진행 상황을 저장(업데이트)합니다.
 *        - 이미 기록이 있으면: 해당 기록을 최신 정보로 업데이트하고 배열의 가장 앞으로 이동시킵니다. (Move-to-Front)
 *        - 기록이 없으면: 새로운 기록을 배열의 가장 앞에 추가합니다.
 *        이 'Move-to-Front' 방식은 최근 항목에 빠르게 접근할 수 있게 해주는 캐시 관리 기법과 유사합니다.
 * @param user 사용자 아이디.
 * @param video 비디오 파일 이름.
 * @param time 현재 재생 시간.
 * @param duration 비디오 총 길이.
 */
void save_history(const char* user, const char* video, double time, double duration) {
    pthread_mutex_lock(&history_mutex);

    // 1. 기존 기록이 있는지 확인
    int idx = -1;
    for (int i = 0; i < history_count; i++) {
        if (strcmp(history[i].username, user) == 0 && strcmp(history[i].video_name, video) == 0) {
            idx = i;
            break;
        }
    }

    // 2. 임시 구조체에 새로운 시청 기록 정보 저장
    WatchHistory temp;
    strncpy(temp.username, user, sizeof(temp.username) - 1);
    temp.username[sizeof(temp.username) - 1] = '\0';
    strncpy(temp.video_name, video, sizeof(temp.video_name) - 1);
    temp.video_name[sizeof(temp.video_name) - 1] = '\0';
    temp.last_time = time;
    temp.duration = duration;

    // 3. 배열 재정렬
    if (idx != -1) {
        // 기존 기록이 있으면 (idx 위치), 0부터 idx-1까지의 모든 항목을 한 칸씩 뒤로 밀고
        if (idx > 0) {
            memmove(&history[1], &history[0], idx * sizeof(WatchHistory));
        }
    } else {
        // 새로운 기록이면, 배열이 꽉 차지 않았을 경우 개수를 늘리고 모든 항목을 한 칸씩 뒤로 밈
        int new_count = (history_count < 100) ? history_count + 1 : 100;
        if (new_count > 1) {
            memmove(&history[1], &history[0], (new_count - 1) * sizeof(WatchHistory));
        }
        if (history_count < 100) {
            history_count++;
        }
    }

    // 4. 새로운/업데이트된 기록을 배열의 가장 첫 번째 위치(인덱스 0)에 삽입
    history[0] = temp;
    pthread_mutex_unlock(&history_mutex);
}

/**
 * @brief 특정 사용자의 특정 비디오에 대한 마지막 재생 시간을 조회합니다.
 * @param user 사용자 아이디.
 * @param video 비디오 파일 이름.
 * @return 마지막 재생 시간(초). 기록이 없으면 0.0을 반환.
 */
double get_history(const char* user, const char* video) {
    double time = 0.0;
    pthread_mutex_lock(&history_mutex);

    for(int i=0; i<history_count; i++) {
        if(strcmp(history[i].username, user) == 0 && strcmp(history[i].video_name, video) == 0) {
            time = history[i].last_time;
            break;
        }
    }

    pthread_mutex_unlock(&history_mutex);
    return time;
}


// --- [인증 및 사용자 관리 함수] ---

/**
 * @brief 사용자 데이터베이스 파일(`user.txt`)을 읽어 특정 사용자가 존재하는지 확인합니다.
 * @param user 확인할 사용자 아이디.
 * @return 사용자가 존재하면 1, 존재하지 않거나 파일을 열 수 없으면 0을 반환합니다.
 */
int user_exists(const char *user) {
    FILE *fp = fopen(USER_DB, "r");
    if (!fp) return 0;

    char line[256], u[50], p[50], n[50];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%s %s %s", u, p, n) >= 2) {
            if (strcmp(user, u) == 0) {
                fclose(fp);
                return 1; // 사용자를 찾음
            }
        }
    }
    fclose(fp);
    return 0; // 사용자를 찾지 못함
}

/**
 * @brief 새로운 사용자를 등록합니다.
 *        `user.txt` 파일에 사용자 정보를 추가합니다.
 * @param user 등록할 사용자 아이디.
 * @param pass 등록할 사용자 비밀번호.
 * @param name 등록할 사용자 이름.
 * @return 성공 시 1, 이미 존재하는 사용자일 경우 0, 파일 오류 시 -1을 반환합니다.
 */
int register_user(const char *user, const char *pass, const char *name) {
    pthread_mutex_lock(&file_mutex); // 파일 접근 동기화

    // user_exists는 내부적으로 파일을 열기 때문에, 락이 풀린 상태에서 호출해야 데드락 방지
    // 하지만 이 함수는 file_mutex를 사용하는 다른 함수(update_user_info)와 함께 쓰이므로,
    // register_user 진입점에서 락을 걸어 일관성을 유지하는 것이 더 안전하다.
    // user_exists 내부에서는 락을 잡지 않으므로 데드락은 발생하지 않는다.
    if (user_exists(user)) {
        pthread_mutex_unlock(&file_mutex);
        return 0; // 이미 사용 중인 아이디
    }
    
    // 만약 이전에 동일한 아이디로 기록이 남아있다면 삭제
    remove_user_history(user);

    FILE *fp = fopen(USER_DB, "a"); // append 모드로 파일 열기
    if (!fp) {
        pthread_mutex_unlock(&file_mutex);
        return -1; // 파일 열기 실패
    }
    fprintf(fp, "%s %s %s\n", user, pass, name);
    fclose(fp);

    pthread_mutex_unlock(&file_mutex);
    return 1; // 성공
}

/**
 * @brief 제공된 아이디와 비밀번호가 `user.txt`에 저장된 정보와 일치하는지 확인합니다.
 * @param user 확인할 사용자 아이디.
 * @param pass 확인할 사용자 비밀번호.
 * @return 일치하면 1, 불일치하거나 사용자가 없으면 0을 반환.
 */
int check_credentials(const char *user, const char *pass) {
    FILE *fp = fopen(USER_DB, "r");
    if (!fp) return 0;

    char line[256], u[50], p[50], n[50];
    int auth = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%s %s %s", u, p, n) >= 2) {
            if (strcmp(user, u) == 0 && strcmp(pass, p) == 0) {
                auth = 1; // 아이디와 비밀번호 모두 일치
                break;
            }
        }
    }
    fclose(fp);
    return auth;
}

/**
 * @brief 사용자의 이름 또는 비밀번호를 변경합니다.
 *        기존 파일을 읽어 임시 파일에 변경된 내용을 쓴 뒤, 원본 파일을 대체하는 안전한 방식을 사용합니다.
 * @param user 정보를 변경할 사용자 아이디.
 * @param old_pass 현재 비밀번호 (인증용).
 * @param new_name 변경할 새 이름.
 * @param new_pass 변경할 새 비밀번호 (비어있으면 변경 안함).
 * @return 성공 시 1, 비밀번호 불일치 시 0, 오류 발생 시 -1을 반환.
 */
int update_user_info(const char *user, const char *old_pass, const char *new_name, const char *new_pass) {
    pthread_mutex_lock(&file_mutex);

    char temp_path[256];
    sprintf(temp_path, "%s.tmp", USER_DB);

    FILE *orig_fp = fopen(USER_DB, "r");
    if (!orig_fp) {
        pthread_mutex_unlock(&file_mutex);
        return -1; // 원본 파일을 열 수 없음
    }

    FILE *temp_fp = fopen(temp_path, "w");
    if (!temp_fp) {
        fclose(orig_fp);
        pthread_mutex_unlock(&file_mutex);
        return -1; // 임시 파일을 생성할 수 없음
    }

    char line[256];
    char u[50], p[50], n[50];
    int updated = 0;
    int password_mismatch = 0;

    // 원본 파일을 한 줄씩 읽어 임시 파일에 씀
    while (fgets(line, sizeof(line), orig_fp)) {
        char parse_line[256];
        strcpy(parse_line, line);

        if (sscanf(parse_line, "%s %s %s", u, p, n) >= 2) {
            if (strcmp(user, u) == 0) { // 수정할 사용자를 찾았을 경우
                if (strcmp(p, old_pass) == 0) { // 비밀번호가 일치하는지 확인
                    // 새 정보로 임시 파일에 기록
                    fprintf(temp_fp, "%s %s %s\n", u, (strlen(new_pass) > 0 ? new_pass : p), new_name);
                    updated = 1;
                } else {
                    // 비밀번호 불일치: 원본 줄을 그대로 쓰고 오류 플래그 설정
                    fputs(line, temp_fp);
                    password_mismatch = 1;
                }
            } else {
                fputs(line, temp_fp); // 다른 사용자의 정보는 그대로 임시 파일에 복사
            }
        } else {
            fputs(line, temp_fp); // 파싱할 수 없는 줄은 그대로 복사
        }
    }

    fclose(orig_fp);
    fclose(temp_fp);

    if (password_mismatch) {
        remove(temp_path); // 임시 파일 삭제
        pthread_mutex_unlock(&file_mutex);
        return 0; // 비밀번호 불일치 오류 반환
    }

    if (updated) {
        // 성공적으로 업데이트된 경우, 임시 파일을 원본 파일로 이름 변경 (원자적 연산)
        if (rename(temp_path, USER_DB) != 0) {
            perror("update_user_info: rename failed");
            remove(temp_path);
            pthread_mutex_unlock(&file_mutex);
            return -1; // 이름 변경 실패
        }
    } else {
        // 업데이트할 사용자를 찾지 못한 경우 임시 파일만 삭제
        remove(temp_path);
    }

    pthread_mutex_unlock(&file_mutex);
    return updated ? 1 : -1; // 성공 시 1, 사용자를 못 찾았으면 -1 반환
}


/**
 * @brief 사용자 아이디를 기반으로 `user.txt`에서 해당 사용자의 이름을 찾아 반환합니다.
 * @param user 사용자 아이디.
 * @param out_name 찾은 이름을 저장할 버퍼.
 */
void get_user_name(const char *user, char *out_name) {
    strcpy(out_name, user); // 기본값으로 아이디를 설정
    FILE *fp = fopen(USER_DB, "r");
    if (!fp) return;

    char line[256], u[50], p[50], n[50];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%s %s %s", u, p, n) == 3) { // 이름까지 3개의 필드가 모두 있는지 확인
            if (strcmp(user, u) == 0) { 
                url_decode(out_name, n); // URL 디코딩된 이름을 out_name에 저장
                // 이름 뒤에 붙을 수 있는 개행문자 제거
                size_t len = strlen(out_name);
                for(int i=0; i<len; i++) {
                    if(out_name[i] == '\n' || out_name[i] == '\r') {
                        out_name[i] = '\0';
                        break;
                    }
                }
                break; // 사용자를 찾았으므로 루프 종료
            }
        }
    }
    fclose(fp);
}

// --- [파일 서빙 함수] ---

/**
 * @brief 정적 파일(HTML, CSS, JS, 이미지 등)을 클라이언트에게 전송합니다.
 *        파일 전체를 메모리에 로드하지 않고, `BSIZE` 크기의 청크로 나누어 스트리밍 전송하여 메모리 효율성을 높였습니다.
 * @param ssl 클라이언트와 연결된 SSL 객체.
 * @param path 전송할 파일의 경로.
 * @param mime_type 전송할 파일의 MIME 타입 (예: "text/html", "image/jpeg").
 */
void serve_static_file(SSL *ssl, const char *path, const char *mime_type) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { 
        send_error(ssl, 404, "Not Found", "File not found"); 
        return; 
    }

    // 파일 크기 계산
    fseek(fp, 0, SEEK_END); 
    long size = ftell(fp); 
    fseek(fp, 0, SEEK_SET);

    // HTTP 헤더 전송
    char header[1024];
    sprintf(header, "HTTP/1.1 200 OK\r\n%sContent-Type: %s\r\nContent-Length: %ld\r\nConnection: keep-alive\r\n\r\n", HSTS_HEADER, mime_type, size);
    if (SSL_write(ssl, header, strlen(header)) <= 0) {
        fclose(fp);
        return;
    }

    // 파일을 청크 단위로 읽어 소켓으로 전송
    char buf[BSIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, BSIZE, fp)) > 0) {
        if (SSL_write(ssl, buf, bytes_read) <= 0) {
            break; // 클라이언트가 연결을 끊었을 경우
        }
    }

    fclose(fp);
}

/**
 * @brief 비디오 파일을 클라이언트에게 스트리밍합니다.
 *        HTTP Range 요청을 지원하여, 클라이언트가 비디오의 특정 부분부터 재생(탐색)할 수 있도록 합니다.
 * @param ssl 클라이언트와 연결된 SSL 객체.
 * @param path 전송할 비디오 파일의 경로.
 * @param range 클라이언트가 보낸 HTTP Range 헤더 값 (예: "bytes=1024-").
 */
void serve_video(SSL *ssl, const char *path, const char *range) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { send_error(ssl, 404, "Not Found", "Video not found"); return; }

    fseek(fp, 0, SEEK_END); 
    long file_size = ftell(fp);
    
    long start = 0, end = file_size - 1;
    int status = 200; // 기본 상태는 200 OK (전체 전송)

    // Range 헤더가 있으면 파싱하여 시작과 끝 위치를 결정
    if (range) {
        sscanf(range, "bytes=%ld-%ld", &start, &end);
        status = 206; // 부분 컨텐츠를 전송함을 의미하는 206 Partial Content
    }

    long content_len = end - start + 1;
    fseek(fp, start, SEEK_SET); // 파일 포인터를 시작 위치로 이동

    // HTTP 206 헤더 전송
    char header[2048];
    sprintf(header, "HTTP/1.1 %d %s\r\n%sContent-Type: video/mp4\r\nContent-Length: %ld\r\nAccept-Ranges: bytes\r\nContent-Range: bytes %ld-%ld/%ld\r\nConnection: keep-alive\r\n\r\n", status, (status==206 ? "Partial Content" : "OK"), HSTS_HEADER, content_len, start, end, file_size);
    if (SSL_write(ssl, header, strlen(header)) <= 0) {
        fclose(fp);
        return;
    }

    // 비디오 데이터를 청크 단위로 읽어 소켓으로 전송
    char buf[BSIZE]; 
    long total_sent = 0;
    while (total_sent < content_len) {
        size_t read_size = (content_len - total_sent > BSIZE) ? BSIZE : (content_len - total_sent);
        size_t r = fread(buf, 1, read_size, fp);
        if (r <= 0) break; // 파일 읽기 오류 또는 완료

        if (SSL_write(ssl, buf, r) <= 0) break; // 소켓 쓰기 오류 (클라이언트 연결 끊김)
        
        total_sent += r;
    }
    fclose(fp);
}

// --- [메인 요청 핸들러] ---

/**
 * @brief 클라이언트 한 명의 연결을 독립적으로 처리하는 메인 함수.
 *        새로운 스레드에서 실행되며, 클라이언트의 전체 생명주기를 관리합니다.
 *        1. SSL 핸드셰이크
 *        2. HTTP 요청 수신 및 파싱
 *        3. 요청 경로(path)에 따른 라우팅 및 비즈니스 로직 처리
 *        4. HTTP 응답 생성 및 전송
 *        5. 연결 종료 및 자원 해제
 * @param arg `client_info` 구조체에 대한 포인터.
 * @return NULL을 반환하며 스레드를 종료합니다.
 */
void *handle_client(void *arg) {
    struct client_info *client = (struct client_info *)arg;
    int s = client->socket;
    
    // 클라이언트 IP 주소 문자열로 변환
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->address.sin_addr, client_ip, INET_ADDRSTRLEN);

    // --- 1. SSL 핸드셰이크 ---
    SSL *ssl = SSL_new(client->ctx);
    SSL_set_fd(ssl, s);
    if (SSL_accept(ssl) <= 0) {
        // 핸드셰이크 실패 시 자원 해제 후 스레드 종료
        ERR_print_errors_fp(stderr);
        goto cleanup;
    }

    // --- 2. HTTP 요청 수신 ---
    char req[16384] = {0}; // 16KB 버퍼
    int total = 0;
    // select()를 사용하여 타임아웃(3초)을 설정하고 non-blocking 방식으로 요청을 읽음
    while (total < 16383) {
        fd_set readfds; 
        FD_ZERO(&readfds); 
        FD_SET(s, &readfds);
        struct timeval timeout = { 3, 0 }; // 3초 타임아웃
        if (select(s + 1, &readfds, NULL, NULL, &timeout) <= 0) break; // 타임아웃 또는 오류
        
        int len = SSL_read(ssl, req + total, 16383 - total);
        if (len <= 0) break; // 연결 끊김 또는 오류
        total += len;
        
        // 헤더의 끝을 나타내는 "\r\n\r\n"이 수신되면 헤더 읽기 중단
        if (strstr(req, "\r\n\r\n")) break; 
    }
    if (total == 0) goto cleanup; // 수신한 데이터가 없으면 종료

    // POST 요청 등 body가 있는 경우, Content-Length 만큼 추가로 읽음
    char *cl_ptr = strcasestr(req, "Content-Length:"); 
    int content_len = cl_ptr ? atoi(cl_ptr + 15) : 0;
    char *body_start = strstr(req, "\r\n\r\n");
    if (body_start && content_len > 0) {
        int head_len = (body_start - req) + 4;
        int body_got = total - head_len;
        while (body_got < content_len && total < 16383) {
            int len = SSL_read(ssl, req + total, 16383 - total);
            if (len <= 0) break;
            total += len; 
            body_got += len;
        }
    }

    // --- 3. HTTP 요청 파싱 ---
    char method[10]={0}, path[1024]={0}, protocol[10]={0};
    sscanf(req, "%s %s %s", method, path, protocol);
    char decoded_path[1024]; 
    url_decode(decoded_path, path); // URL 디코딩

    // 필요한 헤더 정보 추출
    char *range = strstr(req, "Range:"); 
    if (range) { range = strchr(range, '='); if(range) range++; }
    
    char *cookie = strstr(req, "Cookie:"); 
    char current_user[50] = "";
    if (cookie) {
        char *p = strstr(cookie, "user=");
        if (p) sscanf(p + 5, "%[^; \r\n]", current_user); // 쿠키에서 사용자 아이디 추출
    }

    // --- 4. 라우팅 및 비즈니스 로직 처리 ---
    // 요청된 메소드와 경로에 따라 적절한 핸들러 실행

    // --- 정적 파일 서빙 ---
    if (!strcmp(method, "GET") && !strcmp(path, "/")) {
        serve_static_file(ssl, "./public/index.html", "text/html; charset=utf-8");
    }
    else if (!strcmp(method, "GET") && !strcmp(path, "/register.html")) {
        serve_static_file(ssl, "./public/register.html", "text/html; charset=utf-8");
    }
    else if (!strncmp(path, "/videos/", 8)) { // "/videos/..." 로 시작하는 모든 경로
        char fp[4096];
        sprintf(fp, "./public%s", decoded_path);
        serve_video(ssl, fp, range);
    }
    else if (!strncmp(path, "/thumbnails/", 12)) { // "/thumbnails/..." 로 시작하는 모든 경로
        char fp[4096];
        sprintf(fp, "./public%s", decoded_path);
        serve_static_file(ssl, fp, "image/jpeg");
    }

    // --- 사용자 인증 관련 ---
    else if (!strcmp(method, "POST") && !strcmp(path, "/register")) {
        // 회원가입 처리
        char *body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            char n[50]={0}, u[50]={0}, p[50]={0}, p_dec[50]={0}, n_dec[50]={0};
            char *n_loc = strstr(body, "fullname="); 
            char *u_loc = strstr(body, "username="); 
            char *p_loc = strstr(body, "password=");
            if (n_loc && u_loc && p_loc) {
                sscanf(n_loc + 9, "%[^& \r\n]", n); 
                sscanf(u_loc + 9, "%[^& \r\n]", u); 
                sscanf(p_loc + 9, "%[^& \r\n]", p);
                url_decode(p_dec, p); url_decode(n_dec, n);
                
                int result = register_user(u, p_dec, n_dec); // 사용자 등록 시도
                if (result == 1) { // 성공
                    log_message("[%s] Register: %s (Name: %s) | IP: %s\n", get_current_timestamp(), u, n_dec, client_ip);
                    char r[]="<html><head><meta charset='utf-8'><script>alert('회원가입이 성공적으로 완료되었습니다!');location.href='/';</script></head><body></body></html>"; 
                    SSL_write(ssl, "HTTP/1.1 200 OK\r\n\r\n", 19); SSL_write(ssl, r, strlen(r));
                } else { // 실패 (이미 존재하는 아이디)
                    char r[]="<html><head><meta charset='utf-8'><script>alert('회원가입 실패: 이미 사용 중인 아이디입니다.');history.back();</script></head><body></body></html>"; 
                    SSL_write(ssl, "HTTP/1.1 200 OK\r\n\r\n", 19); SSL_write(ssl, r, strlen(r));
                }
                goto cleanup;
            }
        }
        send_error(ssl, 400, "Bad Request", "Invalid registration form data.");
    }
    else if (!strcmp(method, "POST") && !strcmp(path, "/login")) {
        // 로그인 처리
        char *body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            char u[50]={0}, p[50]={0}, p_dec[50]={0};
            char *u_loc = strstr(body, "username="); 
            char *p_loc = strstr(body, "password=");
            if (u_loc && p_loc) {
                sscanf(u_loc + 9, "%[^& \r\n]", u); 
                sscanf(p_loc + 9, "%[^& \r\n]", p);
                url_decode(p_dec, p);
                
                if (check_credentials(u, p_dec)) { // 아이디/비밀번호 확인
                    // 로그인 성공
                    char full_user_name[50];
                    get_user_name(u, full_user_name);
                    log_message("[%s] User: %s (%s) | IP: %s | Action: LOGIN_SUCCESS\n", get_current_timestamp(), u, full_user_name, client_ip);
                    
                    // 쿠키를 설정하고 /list 페이지로 리다이렉트
                    char r[512]; 
                    sprintf(r, "HTTP/1.1 302 Found\r\n%sSet-Cookie: user=%s; Path=/; Max-Age=3600; SameSite=Lax; Secure\r\nLocation: /list\r\n\r\n", HSTS_HEADER, u);
                    SSL_write(ssl, r, strlen(r));
                } else {
                    // 로그인 실패
                    log_message("[%s] User: %s | IP: %s | Action: LOGIN_FAILED (Wrong Credentials)\n", get_current_timestamp(), u, client_ip);
                    char r[]="<html><head><meta charset='utf-8'><script>alert('로그인 실패: 아이디 또는 비밀번호를 확인해주세요.');window.location.href='/';</script></head><body></body></html>"; 
                    SSL_write(ssl, "HTTP/1.1 200 OK\r\n\r\n", 19); SSL_write(ssl, r, strlen(r));
                }
                goto cleanup;
            }
        }
        send_error(ssl, 401, "Unauthorized", "Login Failed");
    }
    else if (!strcmp(method, "GET") && !strcmp(path, "/logout")) {
        // 로그아웃 처리
        if (strlen(current_user) > 0) {
            char full_user_name[50];
            get_user_name(current_user, full_user_name);
            log_message("[%s] User: %s (%s) | IP: %s | Action: LOGOUT_SUCCESS\n", get_current_timestamp(), current_user, full_user_name, client_ip);
        }
        // 쿠키를 삭제하고 메인 페이지로 리다이렉트
        char r[512]; 
        sprintf(r, "HTTP/1.1 302 Found\r\n%sSet-Cookie: user=; Max-Age=0; Path=/; SameSite=Lax; Secure\r\nLocation: /\r\n\r\n", HSTS_HEADER);
        SSL_write(ssl, r, strlen(r));
    }
    
    // --- 동적 페이지 생성 ---
    else if (!strncmp(path, "/list", 5)) {
        // 비디오 목록 페이지 생성
        // 1. 로그인 여부 및 사용자 유효성 확인
        if (strlen(current_user) == 0 || !user_exists(current_user)) {
             // 로그인이 안되어 있거나 쿠키가 위조된 경우, 쿠키를 삭제하고 로그인 페이지로 보냄
             char r[512]; sprintf(r, "HTTP/1.1 302 Found\r\n%sSet-Cookie: user=; Max-Age=0; Path=/; SameSite=Lax; Secure\r\nLocation: /\r\n\r\n", HSTS_HEADER);
            SSL_write(ssl, r, strlen(r)); goto cleanup;
        }

        // 2. list.html 템플릿 파일 읽기
        FILE *fp = fopen("./public/list.html", "r");
        if(fp) {
            fseek(fp, 0, SEEK_END); long fsize = ftell(fp); rewind(fp);
            char *template = malloc(fsize + 1);
            if(template) {
                fread(template, 1, fsize, fp); template[fsize] = 0;
                
                // 검색어 파싱
                char *search_query = strstr(path, "search=");
                char search_term[100] = {0};
                if (search_query) { sscanf(search_query + 7, "%[^& \r\n]", search_term); url_decode(search_term, search_term); }

                // 3. 동적 HTML 컨텐츠 생성 (이어보기, 전체 목록)
                char *dynamic_html = malloc(1024 * 50); 
                char *html_ptr = dynamic_html;
                *html_ptr = '\0';
                char real_name[50] = {0}; get_user_name(current_user, real_name);

                // 3-1. 이어보기(Resume) 카드 생성
                char *resume_cards_html = malloc(1024 * 20);
                char *resume_ptr = resume_cards_html;
                *resume_ptr = '\0';
                int resume_found = 0;

                pthread_mutex_lock(&history_mutex);
                for (int i = 0; i < history_count; i++) {
                    if (strcmp(history[i].username, current_user) == 0) {
                        char video_path[256];
                        sprintf(video_path, "%s%s", VIDEO_DIR, history[i].video_name);
                        if (access(video_path, F_OK) == 0) { // 파일이 실제로 디스크에 존재하는지 확인
                            resume_found = 1;
                            char vn[256], rt[256], tn[256]; 
                            strcpy(vn, history[i].video_name); strcpy(rt, vn); 
                            char *ext = strrchr(rt, '.'); if(ext) *ext = 0; 
                            strcpy(tn, vn); char *td = strrchr(tn, '.'); if(td) strcpy(td, ".jpg");
                            
                            double dur = history[i].duration; if(dur < 1.0) dur = 180.0;
                            float p = (history[i].last_time * 100.0) / dur; if (p > 100.0) p = 100.0;
                            
                            resume_ptr += sprintf(resume_ptr, "<div class='card resume-card' onclick=\"location.href='/watch?v=%s'\"><img src='/thumbnails/%s' alt='%s'><div class='title-overlay'>%s</div><div class='resume-bar-container'><div class='resume-bar' style='width: %.1f%%;'></div></div></div>", vn, tn, rt, rt, p);
                        }
                    }
                }
                pthread_mutex_unlock(&history_mutex);
                
                // 검색 중이 아닐 때만 '이어보기' 섹션 추가
                if (resume_found && search_term[0] == '\0') {
                    html_ptr += sprintf(html_ptr, "<h2 class='category-title'>%s님이 시청 중인 콘텐츠</h2><div class='scroll-container'>%s</div>", real_name, resume_cards_html);
                }
                free(resume_cards_html);

                // 3-2. 전체 비디오 카드 및 JS 배열 생성
                char *video_cards_html = malloc(1024 * 30);
                char *video_ptr = video_cards_html;
                *video_ptr = '\0';
                char *js_array_str = malloc(10240);
                strcpy(js_array_str, "const heroVideos = [");
                
                DIR *d = opendir(VIDEO_DIR); 
                if (d) {
                    struct dirent *dir;
                    while ((dir = readdir(d)) != NULL) {
                        if (strstr(dir->d_name, ".mp4")) {
                            char temp_js[300]; sprintf(temp_js, "'%s',", dir->d_name); strcat(js_array_str, temp_js);
                            // 검색어가 없거나, 파일 이름에 검색어가 포함된 경우 카드 생성
                            if (search_term[0] == '\0' || strcasestr(dir->d_name, search_term) != NULL) {
                                char thumb[256], title[256]; 
                                strcpy(thumb, dir->d_name); char *ext = strrchr(thumb, '.'); if(ext) strcpy(ext, ".jpg"); 
                                strcpy(title, dir->d_name); ext = strrchr(title, '.'); if(ext) *ext = 0;
                                video_ptr += sprintf(video_ptr, "<div class='card video-card' onclick=\"location.href='/watch?v=%s'\"><img src='/thumbnails/%s' alt='%s'><div class='title-overlay'>%s</div></div>", dir->d_name, thumb, title, title);
                            }
                        }
                    }
                    closedir(d);
                }
                // JS 배열의 마지막 콤마(,)를 '];'로 교체
                if(strlen(js_array_str) > strlen("const heroVideos = [") ) {
                    js_array_str[strlen(js_array_str) - 1] = ']';
                    strcat(js_array_str, ";");
                } else {
                    strcat(js_array_str, "];");
                }
                
                html_ptr += sprintf(html_ptr, "<h2 class='category-title'>%s</h2><div class='grid-container'>%s</div>", search_term[0] != '\0' ? "검색 결과" : "전체 콘텐츠", video_cards_html);
                free(video_cards_html);

                // 4. 템플릿과 동적 컨텐츠를 결합하여 Chunked-Encoding으로 전송
                // (큰 HTML을 한 번에 보내지 않고 조각내어 보내 로딩 시간 단축)
                char *p1 = strstr(template, "{{USER_NAME}}");
                char *p2 = strstr(template, "{{VIDEO_LIST}}");
                char *p3 = strstr(template, "{{HERO_VIDEOS_JS_ARRAY}}");
                
                char header_resp[1024]; 
                sprintf(header_resp, "HTTP/1.1 200 OK\r\n%sContent-Type: text/html; charset=utf-8\r\nTransfer-Encoding: chunked\r\n\r\n", HSTS_HEADER);
                SSL_write(ssl, header_resp, strlen(header_resp));
                
                // 데이터를 청크로 보내는 로컬 함수
                void send_chunk(SSL* s, const char* data) { 
                    if(!data || !strlen(data)) return; 
                    char chunk_size_hex[20];
                    sprintf(chunk_size_hex, "%lx\r\n", strlen(data)); 
                    SSL_write(s, chunk_size_hex, strlen(chunk_size_hex)); 
                    SSL_write(s, data, strlen(data)); 
                    SSL_write(s, "\r\n", 2); 
                }

                char *curr = template;
                if (p1 && p2 && p3) {
                    *p1 = 0; send_chunk(ssl, curr); send_chunk(ssl, real_name); curr = p1 + 13; 
                    p2 = strstr(curr, "{{VIDEO_LIST}}"); 
                    if(p2) { *p2 = 0; send_chunk(ssl, curr); send_chunk(ssl, dynamic_html); curr = p2 + 14; }
                    p3 = strstr(curr, "{{HERO_VIDEOS_JS_ARRAY}}");
                    if(p3) { *p3 = 0; send_chunk(ssl, curr); send_chunk(ssl, js_array_str); curr = p3 + 24; }
                } 
                send_chunk(ssl, curr); // 나머지 템플릿 부분 전송
                SSL_write(ssl, "0\r\n\r\n", 5); // 전송 종료 청크
                
                free(template);
                free(dynamic_html);
                free(js_array_str);
            }
            fclose(fp);
        } else { send_error(ssl, 500, "Internal Server Error", "Template file 'list.html' not found."); }
    }
    else if (!strncmp(path, "/watch", 6)) {
        // 비디오 시청 페이지 생성
        char *v = strstr(path, "v="); 
        char ve[256]={0}, vn[256]={0}; 
        if (v) { strcpy(ve, v+2); url_decode(vn, ve); }
        
        double st = get_history(current_user, vn); // 시청 기록 조회
        
        log_message("[%s] User: %s | IP: %s | Action: WATCH_VIDEO | Video: %s\n", get_current_timestamp(), current_user, client_ip, vn);

        // 시청 페이지 HTML 동적 생성
        char body[8192];
        sprintf(body, 
            "<html><head><title>Watch</title><style>:root{--primary:#FFB74D;--bg:#1A1A1A;--text:#E0E0E0;}body{background:var(--bg);color:var(--text);margin:0;height:100vh;display:flex;justify-content:center;align-items:center;font-family:'Gowun Dodum';}.video-container{position:relative;width:80%%;max-width:1200px;}video{width:100%%;max-height:90vh;box-shadow:0 0 20px rgba(255,183,77,0.2);display:block;border-radius:8px;}.close-btn{position:absolute;top:-40px;right:0;color:var(--text);text-decoration:none;font-size:2em;font-weight:bold;z-index:200;}#modal{display:none;position:fixed;top:0;left:0;width:100%%;height:100%%;background:rgba(26,26,26,0.9);justify-content:center;align-items:flex-start;padding-top:200px;z-index:100;}.modal-content{background:rgba(26,26,26,0.95);padding:30px;border-radius:10px;text-align:center;width:300px;}.modal-btn{display:block;width:100%%;padding:15px;margin:10px 0;border:none;border-radius:8px;font-weight:bold;cursor:pointer;font-size:1em;}.btn-resume{background:var(--primary);color:white;}.btn-restart{background:#444;color:var(--text);}</style></head><body>"
            "<div class='video-container'><a href='/list' class='close-btn'>X</a><video id='vid' controls><source src='/videos/%s' type='video/mp4'></video></div>"
            "<div id='modal'><div class='modal-content'><h3>이어보기</h3><p>마지막: <span id='time-display'></span></p><button class='modal-btn btn-resume' onclick='resume()'>계속 보기</button><button class='modal-btn btn-restart' onclick='restart()'>처음부터</button></div></div>"
            "<script>var vid=document.getElementById('vid');var modal=document.getElementById('modal');var savedTime=%.2f;window.onload=function(){if(savedTime>1){document.getElementById('time-display').innerText=Math.floor(savedTime/60)+'분 '+Math.floor(savedTime%%60)+'초';modal.style.display='flex';}else{vid.play();}};function resume(){modal.style.display='none';vid.currentTime=savedTime;vid.play();}function restart(){modal.style.display='none';vid.currentTime=0;vid.play();}setInterval(function(){var safeName=encodeURIComponent('%s'); if(!vid.paused && vid.duration > 0) fetch('/save_progress?video='+safeName+'&time='+vid.currentTime+'&duration='+vid.duration);},1000);</script></body></html>", ve, st, vn);
        
        char h[512]; 
        sprintf(h, "HTTP/1.1 200 OK\r\n%sContent-Type: text/html; charset=utf-8\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", HSTS_HEADER, strlen(body)); 
        SSL_write(ssl, h, strlen(h)); 
        SSL_write(ssl, body, strlen(body));
    }
    // --- AJAX 및 기타 API ---
    else if (!strncmp(path, "/save_progress", 14)) {
        // 비디오 시청 진행상황을 AJAX로 저장
        if (strlen(current_user) > 0) {
            char *q = strchr(path, '?');
            if (q) {
                char v[100]={0}; double t=0, d=180.0;
                char *vp=strstr(q, "video="); char *tp=strstr(q, "time="); char *dp=strstr(q, "duration=");
                if (vp) sscanf(vp+6, "%[^&]", v); 
                if (tp) t = atof(tp+5); 
                if (dp) d = atof(dp+9);
                char cv[100]; url_decode(cv, v); 
                save_history(current_user, cv, t, d); // 시청 기록 저장
            }
        }
        SSL_write(ssl, "HTTP/1.1 200 OK\r\n\r\nOK", 19); // 간단한 응답 전송
    }
    else {
        // 위에서 처리되지 않은 모든 경로는 404 Not Found 오류 처리
        send_error(ssl, 404, "Not Found", "The requested page was not found on this server.");
    }

// --- 5. 연결 종료 및 자원 해제 ---
cleanup:
    if (ssl) {
        SSL_shutdown(ssl); // SSL 연결 정상 종료
        SSL_free(ssl);     // SSL 객체 해제
    }
    if (s >= 0) close(s);       // 소켓 닫기
    if (client) free(client);   // client_info 구조체 메모리 해제
    return NULL;
}