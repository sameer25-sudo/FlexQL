#include "flexql.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <sstream>

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

    char buffer[4096];  // bigger buffer
    int len = recv(db->sock, buffer, sizeof(buffer)-1, 0);
    if (len <= 0) return FLEXQL_ERROR;

    buffer[len] = '\0';

    std::string response(buffer);

    // handle ERROR / EMPTY directly
    if (response == "ERROR\n") {
        return FLEXQL_ERROR;
    }
    else if (response == "EMPTY\n") {
        return FLEXQL_OK;
    }

    std::stringstream ss(response);
    std::string line;

    while (std::getline(ss, line)) {
        if (line.empty()) continue;

        std::vector<std::string> cols;
        std::stringstream row_stream(line);
        std::string cell;

        while (std::getline(row_stream, cell, '|')) {
            cols.push_back(cell);
        }

        // prepare char* array
        std::vector<char*> ccols;
        for (auto &c : cols)
            ccols.push_back(const_cast<char*>(c.c_str()));

        if (callback)
            callback(arg, ccols.size(), ccols.data());
    }

    return FLEXQL_OK;
}

void flexql_free(char* ptr) {
    free(ptr);
}