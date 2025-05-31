#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/inotify.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_PATH BUFFER_SIZE
#define TRUE 1
#define FALSE 0
// Định nghĩa các biến toàn cục để theo dõi trạng thái của quá trình đồng bộ hóa.
uint8_t folder_existed = TRUE;
uint8_t file_existed = TRUE;
uint8_t file_no_change = TRUE;
uint8_t receive_folder_done = FALSE;

// Đây là struct chứa đầy đủ thông tin để kiểm tra thay đổi file: tên, đường dẫn, kích thước, hash, thời gian chỉnh sửa.
typedef struct {
    char filename[1024];
    char filepath[1024];
    long filesize;
    unsigned char hash[16];
    time_t timestamp;
} FileInfo;