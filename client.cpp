#include "flexql.h"
#include <iostream>
using namespace std;

int callback(void* arg, int argc, char** argv) {
    cout << argv[0];
    return 0;
}

int main() {
    FlexQL* db;

    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) {
        cout << "Connection failed\n";
        return 0;
    }

    string query;
    while (true) {
        cout << "flexql> ";
        getline(cin, query);

        if (query == "exit") break;

        flexql_exec(db, query.c_str(), callback, nullptr, nullptr);
    }

    flexql_close(db);
}