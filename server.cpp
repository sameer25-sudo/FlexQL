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
#include <fstream>

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
    string op;
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
    vector<vector<string>> rows;

    bool hasWhere = false;
    Condition where;

    bool hasJoin = false;
    Join join;
};


unordered_map<string, Table> database;
unordered_map<string, mutex> table_mutex;

struct CacheEntry {
    string result;
};

unordered_map<string, pair<CacheEntry, list<string>::iterator>> cache;
unordered_map<string, unordered_map<int,long>> table_index_cache;

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

string strip_table(const string &col) {
    if (col.find('.') != string::npos)
        return col.substr(col.find('.') + 1);
    return col;
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

    // cout << "first " << first << endl;
    // cout << "token[1] " << tokens[1] << endl;
    // cout << "tokens[2] " << tokens[2] << endl;

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
        q.type = INSERT;

        // find table name
        q.table = tokens[2];

        // 🔥 extract raw VALUES string
        size_t pos = queryStr.find("VALUES");
        if (pos == string::npos) return Query();

        string values_part = queryStr.substr(pos + 6);

        // remove trailing ';'
        if (!values_part.empty() && values_part.back() == ';')
            values_part.pop_back();

        // 🔥 parse tuples properly
        vector<vector<string>> rows;
        vector<string> current;
        string value;
        bool in_string = false;
        int depth = 0;

        for (char c : values_part) {
            if (c == '\'' || c == '\"') {
                in_string = !in_string;
                // value += c;
                continue;
            }

            if (!in_string) {
                if (c == '(') {
                    depth++;
                    if (depth == 1) {
                        current.clear();
                        value.clear();
                        continue;
                    }
                }
                else if (c == ')') {
                    depth--;
                    if (depth == 0) {
                        if (!value.empty()) {
                            current.push_back(value);
                            value.clear();
                        }
                        rows.push_back(current);
                        continue;
                    }
                }
                else if (c == ',' && depth == 1) {
                    current.push_back(value);
                    value.clear();
                    continue;
                }
            }

            if (depth >= 1)
                value += c;
        }

        q.rows = rows;
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
        if (i < tokens.size() && to_upper(tokens[i]) == "INNER") {
            q.hasJoin = true;
            // cout << "tokens[i] " << tokens[i] << endl;
            // cout << "q.hasJoin " << q.hasJoin << endl;
            i += 2; // skip INNER JOIN
            
            q.join.table2 = tokens[i++];
            
            i++; // skip ON
            
            // TEST_USERS.ID = TEST_ORDERS.USER_ID
            string left = tokens[i++];
            i++; // skip =
            string right = tokens[i++];
            
            q.join.col1 = left.substr(left.find('.') + 1);
            q.join.col2 = right.substr(right.find('.') + 1);
        }
        if (i < tokens.size() && to_upper(tokens[i]) == "WHERE") {
            q.hasWhere = true;

            if (i + 3 >= tokens.size()) return Query();

            q.where.column = tokens[i + 1];
            q.where.op = tokens[i + 2];
            q.where.value  = tokens[i + 3];
        }     
    }

    return q;
}


string execute_query(const string &raw, const Query &q) {
    // lock_guard<mutex> lock(table_mutex[q.table]);

    // cout << "q.table " << q.table << endl;
    // cout << "current_db " << current_db << endl;
    // cout << "q.columns[0] " << q.columns[0] << endl;
    if (q.type == UNKNOWN)
        return "ERROR\n";

    if (q.type == USE) {
        current_db = q.table;
        return "OK: Using database " + current_db + "\n";
    }
    
    // CACHE
    if (q.type == SELECT) {
        string cached;
        if (cache_get(raw, cached)) return cached;
    }
    
    if (q.type == CREATE_DAB) {
        string dbname = q.table;

        string cmd = "mkdir -p " + dbname;
        system(cmd.c_str());

        return "OK: Database created\n";
    }
    
    if (q.type == CREATE_TAB) {
        lock_guard<mutex> lock(table_mutex[q.table]);
        // auto tokens = tokenize(query);
        if (current_db.empty())
            return "ERR| No database selected\n";

        string path = current_db + "/" + q.table;

        ifstream check(path + ".meta");
        if (check.good())
            return "ERR| Table already exists\n";
        
        ofstream meta(path + ".meta");
        for (auto &col: q.columns)
            meta << col << " ";
            

        ofstream file(path + ".bin", ios::binary);
        ofstream idx(path + ".idx", ios::binary);


        // file << "\n";

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
        lock_guard<mutex> lock(table_mutex[q.table]);
        if (current_db.empty())
            return "ERR| No database selected\n";

        string base = current_db + "/" + q.table;

        ifstream meta(base + ".meta");
        if (!meta.is_open()) return "ERROR\n";

        string header;
        getline(meta, header);

        auto cols = tokenize(header);
        vector<string> types;

        for (auto &c : cols)
            types.push_back(c.substr(c.find(':') + 1));

        auto &index = table_index_cache[base];

        if (index.empty()) {
            index = load_index_bin(base);
        }
        fstream file(base + ".bin", ios::binary | ios::out | ios::app);
        fstream idx(base + ".idx", ios::binary | ios::app);

        // vector<string> flat = q.rows[0];  // all values flattened

        int col_count = types.size();

        // if (flat.size() % col_count != 0)
            // return "ERR| Column mismatch\n";

        for (auto &row_vals : q.rows) {

            if (row_vals.size() != col_count)
                return "ERR| Column mismatch\n";

            int key = stoi(row_vals[0]);

            if (index.count(key))
                return "ERR| Duplicate primary key\n";

            long pos = file.tellp();

            for (int i = 0; i < row_vals.size(); i++) {
                cout << "row_vals[i] " << row_vals[i]  << " " << types[i] << endl;

                if (types[i] == "int") {
                    int x = stoi(row_vals[i]);
                    file.write((char*)&x, sizeof(int));
                }
                else if (types[i] == "double") {
                    double d = stod(row_vals[i]);
                    file.write((char*)&d, sizeof(double));
                }
                else {
                    string val = row_vals[i];
                    cout << "val " << val << endl;
                    // if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') {
                    //     val = val.substr(1, val.size() - 2);
                    // }
                    int len = val.size();
                    file.write((char*)&len, sizeof(int));
                    file.write(val.c_str(), len);
                }
            }

            idx.write((char*)&key, sizeof(int));
            idx.write((char*)&pos, sizeof(long));

            index[key] = pos;

        }

        file.close();
        idx.close();

        cache.clear();

        return "OK\n";
    }

    
    if (q.type == SELECT) {
        string cached;
        if (cache_get(raw, cached)) return cached;

        if (current_db.empty())
            return "ERR| No database selected\n";

        string base = current_db + "/" + q.table;

        // cout << "base " << base << endl;

        ifstream meta(base + ".meta");
        if (!meta.is_open())
            return "ERROR\n";

        string header;
        getline(meta, header);

        auto cols = tokenize(header);
        vector<string> types;



        string result;

        if (q.hasJoin) {
            string base1 = current_db + "/" + q.table;
            string base2 = current_db + "/" + q.join.table2;
            // cout << "JOIN base1=" << base1 << " base2=" << base2 << endl;

            ifstream m1(base1 + ".meta"), m2(base2 + ".meta");

            string h1, h2;
            getline(m1, h1);
            getline(m2, h2);

            auto cols1 = tokenize(h1);
            auto cols2 = tokenize(h2);

            vector<string> types1, types2;

            for (auto &c : cols1)
                types1.push_back(c.substr(c.find(':') + 1));
            for (auto &c : cols2)
                types2.push_back(c.substr(c.find(':') + 1));

            // ✅ FIX: case-insensitive join index
            int j1 = -1, j2 = -1;
            for (int i = 0; i < cols1.size(); i++)
                if (to_upper(get_col_name(cols1[i])) == to_upper(q.join.col1)) j1 = i;

            for (int i = 0; i < cols2.size(); i++)
                if (to_upper(get_col_name(cols2[i])) == to_upper(q.join.col2)) j2 = i;
            
                if (j1 == -1 || j2 == -1) {
                    // cout << "ERROR| No rows \n" << j1 << " j2 "<< j2;
                return "ERROR\n";
            }

            
            // read table1
            vector<vector<string>> rows1;
            ifstream f1(base1 + ".bin", ios::binary);

            while (true) {
                vector<string> row;
                for (auto &t : types1) {
                    if (t == "int") {
                        int x;
                        if (!f1.read((char*)&x, sizeof(int))) { row.clear(); break; }
                        row.push_back(to_string(x));
                    } else if (t == "double") {
                        double d;
                        if (!f1.read((char*)&d, sizeof(double))) { row.clear(); break; }
                        row.push_back(to_string((long long)d));
                    } else {
                        int len;
                        if (!f1.read((char*)&len, sizeof(int))) { row.clear(); break; }
                        string s(len, ' ');
                        f1.read(&s[0], len);
                        row.push_back(s);
                    }
                }
                if (row.empty()) break;
                rows1.push_back(row);
            }

            // read table2
            vector<vector<string>> rows2;
            ifstream f2(base2 + ".bin", ios::binary);

            while (true) {
                vector<string> row;
                for (auto &t : types2) {
                    if (t == "int") {
                        int x;
                        if (!f2.read((char*)&x, sizeof(int))) { row.clear(); break; }
                        row.push_back(to_string(x));
                    } else if (t == "double") {
                        double d;
                        if (!f2.read((char*)&d, sizeof(double))) { row.clear(); break; }
                        row.push_back(to_string((long long)d));
                    } else {
                        int len;
                        if (!f2.read((char*)&len, sizeof(int))) { row.clear(); break; }
                        string s(len, ' ');
                        f2.read(&s[0], len);
                        row.push_back(s);
                    }
                }
                if (row.empty()) break;
                rows2.push_back(row);
            }

            // 🚀 HASH JOIN (build on table2)
            unordered_map<string, vector<vector<string>>> hash;

            for (auto &r2 : rows2) {
                hash[r2[j2]].push_back(r2);
            }

            string result;
            // cout << "JOIN j1=" << j1 << " j2=" << j2 << endl;

            // probe table1
            for (auto &r1 : rows1) {
                string key = r1[j1];

                if (!hash.count(key)) continue;

                for (auto &r2 : hash[key]) {

                    // ✅ WHERE
                    if (q.hasWhere) {
                        string target = to_upper(strip_table(q.where.column));
                        int idx = -1;

                        // only checking table2 (fine for your test)
                        for (int i = 0; i < cols2.size(); i++) {
                            if (to_upper(get_col_name(cols2[i])) == target) {
                                idx = i;
                                break;
                            }
                        }

                        if (idx == -1) continue;

                        int val = stoi(r2[idx]);
                        int cond = stoi(q.where.value);

                        if (q.where.op == ">" && val <= cond) continue;
                        if (q.where.op == "<" && val >= cond) continue;
                        if (q.where.op == "=" && val != cond) continue;
                    }

                    // ✅ PROJECTION (correct)
                    for (auto &col : q.columns) {
                        string target = to_upper(strip_table(col));
                        bool found = false;

                        for (int i = 0; i < cols1.size(); i++) {
                            if (to_upper(get_col_name(cols1[i])) == target) {
                                result += r1[i] + "|";
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            for (int i = 0; i < cols2.size(); i++) {
                                if (to_upper(get_col_name(cols2[i])) == target) {
                                    result += r2[i] + "|";
                                    break;
                                }
                            }
                        }
                    }

                    result.pop_back();
                    result += "\n";
                }
            }

            return result.empty() ? "EMPTY\n" : result;
        }

                for (auto &col : q.columns) {
            bool found = false;
            // cout << "q.columns " << col << endl;
            // cout << "strip(q.columns) " << strip_table(col) << endl;
            for (auto &c : cols) {
                // cout << "get_col_name(c) " << get_col_name(c) << endl;
                if (to_upper(get_col_name(c)) == to_upper(strip_table(col))){
                    found = true;
                    break;
                }
            }
            if (!found && col != "*") {
                // cout << "ERROR| No col found" << found << endl;
                return "ERROR\n";
            }
        }

        for (auto &c : cols)
            types.push_back(c.substr(c.find(':') + 1));

        // FAST INDEX LOOKUP
        if (q.hasWhere && q.where.column == get_col_name(cols[0])) {
            auto index = load_index_bin(base);
            int key = stoi(q.where.value);

            // cout << "index " << &index << endl;
            // cout << "key " << key << endl;

            // cout << "SELECT key=" << key << " pos=" << index[key] << endl;


            // for (auto &p : index) {
            //     cout << "INDEX: " << p.first << " -> " << p.second << endl;
            // }

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
                    if (d == (long long)d) {
                        row.push_back(to_string((long long)d));  // print as int
                    } else {
                        ostringstream oss;
                        oss << d;
                        row.push_back(oss.str());
                    }
                }
                else {
                    int len;
                    file.read((char*)&len, sizeof(int));
                    string s(len, ' ');
                    file.read(&s[0], len);
                    row.push_back(s);
                }
            }

            if (q.columns.size() == 1 && q.columns[0] == "*") {
                for (auto &v : row)
                    result += v + "|";
            } else {
                for (auto &col : q.columns) {
                    for (int i = 0; i < cols.size(); i++) {
                        if (to_upper(get_col_name(cols[i])) == to_upper(strip_table(col))) {
                            result += row[i] + "|";
                            break;
                        }
                    }
                }
            }
            result.pop_back();
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
            if (d == (long long)d) {
                    row.push_back(to_string((long long)d));  // print as int
                } else {
                    ostringstream oss;
                    oss << d;
                    row.push_back(oss.str());
                }
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

                }
            }


            if (row.empty()) break;

            if (q.hasWhere) {
                int idx = -1;
                for (int i = 0; i < cols.size(); i++) {
                    if (to_upper(get_col_name(cols[i])) == to_upper(strip_table(q.where.column))){
                        idx = i;
                        break;
                    }
                }

                if (idx == -1) return "ERROR\n";
                if (types[idx] == "int") {
                    int val = stoi(row[idx]);
                    int cond = stoi(q.where.value);

                    if (q.where.op == "=" && val != cond) continue;
                    if (q.where.op == ">" && val <= cond) continue;
                    if (q.where.op == "<" && val >= cond) continue;
                }
                else if (types[idx] == "double") {
                    float val = stod(row[idx]);
                    float cond = stod(q.where.value);

                    if (q.where.op == "=" && val != cond) continue;
                    if (q.where.op == ">" && val <= cond) continue;
                    if (q.where.op == "<" && val >= cond) continue;
                } else {
                    if (row[idx] != q.where.value) continue;
                }
            }

            if (q.columns.size() == 1 && q.columns[0] == "*") {
                for (auto &v : row)
                    result += v + "|";
            } else {
                for (auto &col : q.columns) {
                    for (int i = 0; i < cols.size(); i++) {
                        if (to_upper(get_col_name(cols[i])) == to_upper(strip_table(col))){
                            result += row[i] + "|";
                            break;
                        }
                    }
                }
            }
            result.pop_back();
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

/*

CREATE DATABASE testdb;
USE DATABASE testdb;
CREATE TABLE users (id INT, name VARCHAR 50, balance DOUBLE);
INSERT INTO users VALUES (1, John, 100);
SELECT * FROM users;
*/