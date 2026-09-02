#include"schema.hpp"
#include"exceptions.hpp"
#include<algorithm>
#include<cctype>
#include<filesystem>
#include<fstream>
#include<sstream>
#include<vector>

namespace matching_engine{

namespace{

namespace fs = std::filesystem;

std::string readFile(const fs::path& path){
    std::ifstream file(path);
    if(!file.is_open()){
        throw DatabaseError("Cannot open schema file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// true, если строка пуста или состоит только из пробельных символов —
// например, весь файл был закомментирован или обрезан до нуля.
bool isBlank(const std::string& text){
    return std::all_of(text.begin(), text.end(), [](unsigned char ch){
        return std::isspace(ch);
    });
}

}

void applySchema(PgConnection& connection, const std::string& schemaDir){
    std::error_code ec;
    const bool isDirectory = fs::is_directory(schemaDir, ec);
    if(ec){
        throw DatabaseError("Cannot access schema directory '" + schemaDir + "': " +
            ec.message());
    }
    if(!isDirectory){
        throw DatabaseError("Schema directory not found: " + schemaDir);
    }

    std::vector<fs::path> files;
    try{
        for(const auto& entry : fs::directory_iterator(schemaDir)){
            if(entry.is_regular_file() && entry.path().extension() == ".sql"){
                files.push_back(entry.path());
            }
        }
    } catch(const fs::filesystem_error& e){
        throw DatabaseError("Cannot read schema directory '" + schemaDir + "': " +
            e.code().message());
    }

    if(files.empty()){
        throw DatabaseError("No .sql files found in schema directory: " + schemaDir);
    }

    std::sort(files.begin(), files.end());

    for(const auto& file : files){
        std::string content = readFile(file);
        if(isBlank(content)){
            continue;
        }
        connection.executeScript(content);
    }
}

}
