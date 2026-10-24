#pragma once
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif
namespace qbtc {
inline std::filesystem::path file_path(const std::string &s) { std::u8string value; value.reserve(s.size()); for(char c:s)value.push_back(static_cast<char8_t>(static_cast<unsigned char>(c))); return std::filesystem::path(value); }
#ifdef _WIN32
inline int unicode_main(int argc, wchar_t **argv, int (*entry)(int,char**)) {
    std::vector<std::string> values; values.reserve(static_cast<std::size_t>(argc));
    for(int i=0;i<argc;i++){
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if(n<=0)return 64;
        std::string s(static_cast<std::size_t>(n),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,s.data(),n,nullptr,nullptr)!=n)return 64;
        s.pop_back();values.push_back(std::move(s));
    }
    std::vector<char*> args; for(auto &s:values)args.push_back(s.data());args.push_back(nullptr);
    return entry(argc,args.data());
}
#endif
}
#ifdef _WIN32
#define QBTC_ENTRY(entry) int wmain(int argc,wchar_t **argv){return qbtc::unicode_main(argc,argv,entry);}
#else
#define QBTC_ENTRY(entry) int main(int argc,char **argv){return entry(argc,argv);}
#endif
