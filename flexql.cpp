#include "flexql.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

struct FlexQL {
    int sock;
};

int flexql_open(const char* host, int port, FlexQL** db) {
    *db = new FlexQL();

    (*db)->sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, host, &server.sin_addr);

    if (connect((*db)->sock, (sockaddr*)&server, sizeof(server)) < 0)
        return FLEXQL_ERROR;

    return FLEXQL_OK;
}

int flexql_close(FlexQL* db) {
    if (!db) return FLEXQL_ERROR;
    close(db->sock);
    delete db;
    return FLEXQL_OK;
}

int flexql_exec(FlexQL* db, const char* query,
                flexql_callback callback, void* arg,
                char** errmsg) {

    send(db->sock, query, strlen(query), 0);

    char buffer[1024];
    int len = recv(db->sock, buffer, sizeof(buffer)-1, 0);
    buffer[len] = '\0';

    if (callback) {
        char* row[1];
        row[0] = buffer;
        callback(arg, 1, row);
    }

    return FLEXQL_OK;
}

void flexql_free(char* ptr) {
    free(ptr);
}