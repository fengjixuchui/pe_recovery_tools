#pragma once

#include <Windows.h>
#include <TlHelp32.h>

#include <string>
#include <map>
#include <set>

#include "pe_hdrs_helper.h"
#include "pe_raw_to_virtual.h"
#include "exported_func.h"

size_t forwarderNameLen(BYTE* fPtr); 

std::string getDllName(const std::string& str);

std::string getFuncName(const std::string& str);

std::string formatDllFunc(const std::string& str);

size_t make_lookup_tables(std::string moduleName, ULONGLONG remoteBase, PVOID modulePtr, 
                                std::map<std::string, std::set<std::string>> &forwarders_lookup,
                                std::map<ULONGLONG, std::set<std::string>> &va_to_names,
                                std::map<std::string, ULONGLONG> &name_to_va,
                                std::map<ExportedFunc, std::set<ExportedFunc>> &forwarders_lookup2,
                                std::map<ULONGLONG, std::set<ExportedFunc>> &va_to_func,
                                std::map<ExportedFunc, ULONGLONG> &func_to_va
                                );
