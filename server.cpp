#include <iostream>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <thread>
#include <mutex>
#include <arpa/inet.h>
#include <algorithm>
#include <unistd.h>
#include <list>

using namespace std;

struct Row {
    vector<string> values;
};

struct Table {
    vector<string> columns;
    vector<Row> rows;

    unordered_map<string, int> index; // primary key index
};

enum QueryType { USE, CREATE_TAB, CREATE_DAB, INSERT, SELECT, UNKNOWN };

struct Condition {
    string table;
    string column;
    string value;
};

struct Join {
    string table2;
    string col1;
    string col2;
};

struct Query {
    QueryType type = UNKNOWN;
    string table;
    vector<string> columns;
    vector<string> values;

    bool hasWhere = false;
    Condition where;

    bool hasJoin = false;
    Join join;
};


unordered_map<string, Table> database;
mutex db_mutex;

struct CacheEntry {
    string result;
};

unordered_map<string, pair<CacheEntry, list<string>::iterator>> cache;
list<string> lru;
int CACHE_SIZE = 100;
string current_db = "";

void cache_put(const string &key, const string &value) {
    if (cache.count(key))
        lru.erase(cache[key].second);

    lru.push_front(key);
    cache[key] = {{value}, lru.begin()};

    if (cache.size() > CACHE_SIZE) {
        string last = lru.back();
        lru.pop_back();
        cache.erase(last);
    }
}

bool cache_get(const string &key, string &value) {
    if (!cache.count(key)) return false;

    lru.erase(cache[key].second);   
    lru.push_front(key);
    cache[key].second = lru.begin();

    value = cache[key].first.result;
    return true;
}

// ---------------- COMMANDS ----------------

#include <fstream>

string to_upper(string s) {
    transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

string normalize_type(string t) {
    t = to_upper(t);

    if (t == "INT" || t == "INTEGER")
        return "int";

    if (t == "DECIMAL" || t == "DOUBLE")
        return "double";

    if (t.find("CHAR") != string::npos || t == "TEXT" || t == "VARCHAR")
        return "string";

    return "string";
}

vector<string> tokenize(const string &input) {
    vector<string> tokens;
    string token;
    bool in_string = false;

    for (char c : input) {
        if (c == '\'') {
            in_string = !in_string;
            continue;
        }

        if (!in_string && (isspace(c) || c == ',' || c == '(' || c == ')' || c == ';')) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }

    if (!token.empty())
        tokens.push_back(token);

    return tokens;
}

bool is_number(const string &s) {
    for (char c : s)
        if (!isdigit(c)) return false;
    return true;
}

string get_col_name(const string &col) {
    return col.substr(0, col.find(':'));
}

unordered_map<string, long> load_index(const string &path) {
    unordered_map<string, long> index;

    ifstream file(path + ".idx");
    string key;
    long pos;

    while (file >> key >> pos) {
        index[key] = pos;
    }

    return index;
}

unordered_map<int,long> load_index_bin(const string &base) {
    unordered_map<int,long> index;
    ifstream idx(base + ".idx", ios::binary);
    if (!idx.is_open()) return index;

    int key;
    long pos;

    while (idx.read((char*)&key, sizeof(int))) {
        idx.read((char*)&pos, sizeof(long));
        index[key] = pos;
    }

    return index;
}

Query parse_query(const string &queryStr) {
    Query q;
    auto tokens = tokenize(queryStr);
    if (tokens.empty()) return q;

    string first = to_upper(tokens[0]);

    cout << "first " << first << endl;
    cout << "token[1] " << tokens[1] << endl;
    cout << "tokens[2] " << tokens[2] << endl;

    if (first == "USE") {
        q.type = USE;
        q.table = tokens[2];
    }

    else if (first == "CREATE") {
        string second = to_upper(tokens[1]);

        if (second == "TABLE") {
            q.type = CREATE_TAB;

            // table name
            q.table = tokens[2];

            // parse (col type pairs)
            // tokens after table name are like:
            // ID DECIMAL NAME VARCHAR 64 BALANCE DECIMAL ...

            for (size_t i = 3; i < tokens.size();) {
                string col = tokens[i++];

                if (i >= tokens.size())
                    return Query(); // invalid → UNKNOWN

                string type = normalize_type(tokens[i++]);

                // skip size if exists (VARCHAR 64)
                if (i < tokens.size() && is_number(tokens[i])) {
                    i++;
                }

                q.columns.push_back(col + ":" + type);
            }
        }

        else if (second == "DATABASE") {
            q.type = CREATE_DAB;
            q.table = tokens[2];
        }
    }
    
    else if (first == "INSERT") {
        if (tokens.size() < 4) return Query();

        if (to_upper(tokens[1]) != "INTO")
            return Query();

        q.type = INSERT;
        q.table = tokens[2];

        int i = 3;

        if (to_upper(tokens[i]) == "VALUES") i++;

        // remaining tokens = values
        for (; i < tokens.size(); i++)
            q.values.push_back(tokens[i]);
    }


    else if (first == "SELECT") {
        q.type = SELECT;
        int i = 1;

        // columns
        while (i < tokens.size() && to_upper(tokens[i]) != "FROM") {
            q.columns.push_back(tokens[i]);
            i++;
        }

        if (i >= tokens.size()) return Query();

        i++; // skip FROM

        if (i >= tokens.size()) return Query();
        q.table = tokens[i++];

        // WHERE
        if (i < tokens.size() && to_upper(tokens[i]) == "WHERE") {
            q.hasWhere = true;

            if (i + 3 >= tokens.size()) return Query();

            q.where.column = tokens[i + 1];
            q.where.value = tokens[i + 3];
        }
    }

    return q;
}


string execute_query(const string &raw, const Query &q) {
    lock_guard<mutex> lock(db_mutex);

    cout << "q.table " << q.table << endl;
    cout << "current_db " << current_db << endl;
    // cout << "q.columns[0] " << q.columns[0] << endl;
    
    if (q.type == USE) {
        current_db = q.table;
        return "OK: Using database " + current_db + "\n";
    }
    
    // CACHE
    // if (q.type == SELECT) {
    //     string cached;
    //     if (cache_get(raw, cached)) return cached;
    // }
    
    if (q.type == CREATE_DAB) {
        string dbname = q.table;

        string cmd = "mkdir -p " + dbname;
        system(cmd.c_str());

        return "OK: Database created\n";
    }
    
    if (q.type == CREATE_TAB) {

        // auto tokens = tokenize(query);
        if (current_db.empty())
            return "ERROR: No database selected\n";

        string path = current_db + "/" + q.table;

        ifstream check(path + ".meta");
        if (check.good())
            return "ERROR: Table already exists\n";
        
        ofstream meta(path + ".meta");
        for (auto &col: q.columns)
            meta << col << " ";
            

        ofstream file(path + ".bin", ios::binary);
        ofstream idx(path + ".idx", ios::binary);


        file << "\n";

        file.close();
        idx.close();

        return "OK: Table Created\n";

        // cout << "create table \n";
        // Table t;
        // t.columns = q.columns;
        // database[q.table] = t;
        // return "OK\n";
    }

    if (q.type == INSERT) {
        if (current_db.empty())
            return "ERROR: No database selected\n";

        string base = current_db + "/" + q.table;

        ifstream meta(base + ".meta");
        if (!meta.is_open()) return "ERROR: Table not found\n";

        string header;
        getline(meta, header);

        auto cols = tokenize(header);
        vector<string> types;

        for (auto &c : cols)
            types.push_back(c.substr(c.find(':') + 1));

        unordered_map<int,long> index = load_index_bin(base);

        int key = stoi(q.values[0]);
        
        cout << "types[0]" << types[0] << endl;
        
        if (index.count(key))
            return "ERROR: Duplicate primary key\n";

        // 🔥 OPEN WITHOUT ios::app (important)
        fstream file(base + ".bin", ios::binary | ios::in | ios::out);
        if (!file) {
            file.open(base + ".bin", ios::binary | ios::out);
            file.close();
            file.open(base + ".bin", ios::binary | ios::in | ios::out);
        }
        file.seekp(0, ios::end);
        long pos = file.tellp();


        
        if (q.values.size() != types.size())
            return "ERROR: Column mismatch\n";

        // fstream file(base + ".bin", ios::binary | ios::app);
        fstream idx(base + ".idx", ios::binary | ios::app);

        // long pos = file.tellp();

        for (int i = 0; i < q.values.size(); i++) {
            if (types[i] == "int") {
                int x = stoi(q.values[i]);
                file.write((char*)&x, sizeof(int));
            }
            else if (types[i] == "double") {
                double d = stod(q.values[i]);
                file.write((char*)&d, sizeof(double));
            }
            else { // string
                string val = q.values[i];

                int len = val.size();
                file.write((char*)&len, sizeof(int));
                file.write(val.c_str(), len);
            }
        }

        file.close();

        // int key = stoi(q.values[0]);
        idx.write((char*)&key, sizeof(int));
        idx.write((char*)&pos, sizeof(long));

        idx.close();
        cache.clear();
        return "OK\n";
    }


    
    if (q.type == SELECT) {
        string cached;
        if (cache_get(raw, cached)) return cached;

        if (current_db.empty())
            return "ERROR: No database selected\n";

        string base = current_db + "/" + q.table;

        cout << "base " << base << endl;

        ifstream meta(base + ".meta");
        if (!meta.is_open())
            return "ERROR: Table not found\n";

        string header;
        getline(meta, header);

        auto cols = tokenize(header);
        vector<string> types;

        for (auto &c : cols)
            types.push_back(c.substr(c.find(':') + 1));

        string result;

        // FAST INDEX LOOKUP
        if (q.hasWhere && q.where.column == get_col_name(cols[0])) {
            auto index = load_index_bin(base);
            int key = stoi(q.where.value);

            cout << "index " << &index << endl;
            cout << "key " << key << endl;

            if (!index.count(key))
                return "EMPTY\n";

            ifstream file(base + ".bin", ios::binary);
            file.seekg(index[key]);

            vector<string> row;

            for (auto &t : types) {
                if (t == "int") {
                    int x;
                    file.read((char*)&x, sizeof(int));
                    row.push_back(to_string(x));
                }
                else if (t == "double") {
                    double d;
                    if (!file.read((char*)&d, sizeof(double))) {
                        row.clear();
                        break;
                    }
                    row.push_back(to_string(d));
                }
                else {
                    int len;
                    file.read((char*)&len, sizeof(int));
                    string s(len, ' ');
                    file.read(&s[0], len);
                    row.push_back(s);
                }
            }

            for (auto &v : row) result += v + " ";
            result += "\n";
            return result;
        }

        // FULL SCAN
        ifstream file(base + ".bin", ios::binary);

        while (true) {
            vector<string> row;

            for (auto &t : types) {
                if (t == "int") {
                    int x;
                    if (!file.read((char*)&x, sizeof(int))) {
                        row.clear();
                        break;
                    }
                    row.push_back(to_string(x));
                } else if (t == "double") {
                    double d;
                    if (!file.read((char*)&d, sizeof(double))) {
                        row.clear();
                        break;
                    }
                    row.push_back(to_string(d));
                }
                else {
                    int len;
                    if (!file.read((char*)&len, sizeof(int))) {
                        row.clear();
                        break;
                    }
                    string s(len, ' ');
                    file.read(&s[0], len);
                    row.push_back(s);
                    cout << "s " << s << endl;

                }
            }

            for (auto &v : row) {
                cout << "v " << v << endl;
            }

            if (row.empty()) break;

            if (q.hasWhere) {
                int idx = -1;
                for (int i = 0; i < cols.size(); i++) {
                    if (get_col_name(cols[i]) == q.where.column) {
                        idx = i;
                        break;
                    }
                }

                if (idx == -1) return "ERROR\n";
                if (types[idx] == "double") {
                    if (stod(row[idx]) != stod(q.where.value)) continue;
                } else {
                    if (row[idx] != q.where.value) continue;
                }
            }

            for (auto &v : row) {
                cout << "v " << v << endl;
                result += v + " ";
            }
            result += "\n";
        }

        return result.empty() ? "EMPTY\n" : result;
    }  
    

    return "ERROR\n";
}

// ---------------- CLIENT HANDLER ----------------

void handle_client(int client_sock) {
    char buffer[1024];

    while (true) {
        int len = recv(client_sock, buffer, sizeof(buffer)-1, 0);
        if (len <= 0) break;

        string query(buffer, len);
        Query q = parse_query(query);
        string response = execute_query(query, q);

        send(client_sock, response.c_str(), response.size(), 0);
    }

    close(client_sock);
}

// ---------------- MAIN SERVER ----------------

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    server.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&server, sizeof(server));
    listen(server_fd, 5);

    cout << "Server running on port 9000...\n";

    while (true) {
        int client_sock = accept(server_fd, nullptr, nullptr);

        thread t(handle_client, client_sock);
        t.detach();
    }

    return 0;
}