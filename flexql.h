#ifndef FLEXQL_H
#define FLEXQL_H

#include <string>

#define FLEXQL_OK 0
#define FLEXQL_ERROR 1

struct FlexQL;

typedef int (*flexql_callback)(void* arg, int argc, char** argv);

int flexql_open(const char* host, int port, FlexQL** db);
int flexql_close(FlexQL* db);
int flexql_exec(FlexQL* db, const char* query,
                flexql_callback callback, void* arg,
                char** errmsg);
void flexql_free(char* ptr);

#endif