#include "fix_imports.h"
#include <algorithm>

#define MIN_DLL_LEN 5

struct FuncNameLengthCompare
{
    bool operator() (const ExportedFunc &p_lhs, const ExportedFunc &p_rhs)
    {
        const size_t lhsLength = p_lhs.funcName.length();
        const size_t rhsLength = p_rhs.funcName.length();

        if (lhsLength == rhsLength) {
            return (p_lhs.funcName.compare(p_rhs.funcName));
        }
        return (lhsLength < rhsLength); // compares with the length
    }
};

LPVOID search_name(std::string name, const char* modulePtr, size_t moduleSize)
{
    const char* namec = name.c_str();
    const size_t searched_len =  name.length() + 1; // with terminating NULL
    const char* found_ptr = std::search(modulePtr, modulePtr + moduleSize, namec, namec + searched_len);
    if (found_ptr == NULL) {
        return NULL;
    }
    size_t o = found_ptr - modulePtr;
    if (o < moduleSize) {
       return (LPVOID)(found_ptr);
    }
    return NULL;
}

bool findNameInBinaryAndFill(LPVOID modulePtr, size_t moduleSize,
                      IMAGE_IMPORT_DESCRIPTOR* lib_desc,
                      LPVOID call_via_ptr,
                      std::map<ULONGLONG, std::set<ExportedFunc, FuncNameLengthCompare>> &addr_to_func
                      )
{
    if (call_via_ptr == NULL || modulePtr == NULL || lib_desc == NULL) {
        return false; //malformed input
    }
    DWORD *call_via_val = (DWORD*)call_via_ptr;
    if (*call_via_val == 0) {
        //nothing to fill, probably the last record
        return false;
    }
    ULONGLONG searchedAddr = ULONGLONG(*call_via_val);
    bool is_name_saved = false;

    DWORD lastOrdinal = 0; //store also ordinal of the matching function
    std::set<ExportedFunc, FuncNameLengthCompare>::iterator funcname_itr = addr_to_func[searchedAddr].begin();

    for (funcname_itr = addr_to_func[searchedAddr].begin(); 
        funcname_itr != addr_to_func[searchedAddr].end(); 
        funcname_itr++) 
    {
        const ExportedFunc &found_func = *funcname_itr;
        lastOrdinal = found_func.funcOrdinal;

        const char* names_start = ((const char*) modulePtr + lib_desc->Name);
        BYTE* found_ptr = (BYTE*) search_name(found_func.funcName, names_start, moduleSize - (names_start - (const char*)modulePtr));
        if (!found_ptr) {
            //name not found in the binary
            //TODO: maybe it is imported by ordinal?
            continue;
        }
        const DWORD offset = static_cast<DWORD>((ULONGLONG)found_ptr - (ULONGLONG)modulePtr);

        //if it is not the first name from the list, inform about it:
        if (funcname_itr != addr_to_func[searchedAddr].begin()) {
            printf(">[*][%llx] %s\n", searchedAddr, found_func.funcName.c_str());
        }
        printf("[+] Found the name at: %llx\n", static_cast<ULONGLONG>(offset));
        const DWORD name_offset = offset - sizeof(WORD);
        //TODO: validate more...
        memcpy((BYTE*)call_via_ptr, &name_offset, sizeof(DWORD));
        printf("[+] Wrote found to offset: %p\n", call_via_ptr);
        is_name_saved = true;
        break;
    }
    //name not found or could not be saved - filling the ordinal instead:
    if (is_name_saved == false) {
        if (lastOrdinal != 0) {
            printf("[+] Filling ordinal: %x\n", lastOrdinal);
            DWORD ord_thunk = lastOrdinal | IMAGE_ORDINAL_FLAG32;
            memcpy(call_via_ptr, &ord_thunk, sizeof(DWORD)); 
            is_name_saved = true;
        }
    }
    return is_name_saved;
}

bool fillImportNames32(IMAGE_IMPORT_DESCRIPTOR* lib_desc, LPVOID modulePtr, size_t moduleSize,
        std::map<ULONGLONG, std::set<ExportedFunc, FuncNameLengthCompare>> &addr_to_func)
{
    if (lib_desc == NULL) return false;

    DWORD call_via = lib_desc->FirstThunk;
    if (call_via == NULL) return false;

    size_t processed_imps = 0;
    size_t recovered_imps = 0;

    DWORD thunk_addr = lib_desc->OriginalFirstThunk;
    if (thunk_addr == NULL) {
        thunk_addr = call_via;
    }

    do {
        LPVOID call_via_ptr = (LPVOID)((ULONGLONG)modulePtr + call_via);
        if (call_via_ptr == NULL) break;

        LPVOID thunk_ptr = (LPVOID)((ULONGLONG)modulePtr + thunk_addr);
        if (thunk_ptr == NULL) break;

        DWORD *thunk_val = (DWORD*)thunk_ptr;
        DWORD *call_via_val = (DWORD*)call_via_ptr;
        if (*call_via_val == 0) {
            //nothing to fill, probably the last record
            break;
        }

        ULONGLONG searchedAddr = ULONGLONG(*call_via_val);
        std::set<ExportedFunc, FuncNameLengthCompare>::iterator funcname_itr = addr_to_func[searchedAddr].begin();

        if (addr_to_func[searchedAddr].begin() == addr_to_func[searchedAddr].end()) {
            printf("[-] Function not found: %X\n", searchedAddr);
            call_via += sizeof(DWORD);
            thunk_addr += sizeof(DWORD);
            continue;
        }

        std::string found_name = funcname_itr->funcName;
        printf("[*][%llx] %s\n", searchedAddr, found_name.c_str());

        bool is_name_saved = false;

        IMAGE_THUNK_DATA32* desc = (IMAGE_THUNK_DATA32*) thunk_ptr;
        if (desc->u1.Function == NULL) {
            break;
        }

        PIMAGE_IMPORT_BY_NAME by_name = (PIMAGE_IMPORT_BY_NAME) ((ULONGLONG) modulePtr + desc->u1.AddressOfData);
        if (desc->u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
            printf("Imports by ordinals are not supported!\n");
            call_via += sizeof(DWORD);
            thunk_addr += sizeof(DWORD);
            continue;
        }

        DWORD lastOrdinal = 0;
        LPSTR func_name_ptr = by_name->Name;
        bool is_nameptr_valid = validate_ptr(modulePtr, moduleSize, func_name_ptr, found_name.length());
        // try to save the found name under the pointer:
        if (is_nameptr_valid == true) {
            memcpy(func_name_ptr, found_name.c_str(), found_name.length());
            printf("[+] Saved\n");
            is_name_saved = true;
        } else {
            // try to find the offset to the name in the module:
            is_name_saved = findNameInBinaryAndFill(modulePtr, moduleSize, lib_desc, call_via_ptr, addr_to_func);
        }

        call_via += sizeof(DWORD);
        thunk_addr += sizeof(DWORD);
        processed_imps++;
        if (is_name_saved) recovered_imps++;

    } while (true);

    return (recovered_imps == processed_imps);
}

size_t findAddressesToFill32(DWORD call_via, DWORD thunk_addr, LPVOID modulePtr, OUT std::set<ULONGLONG> &addresses)
{
    size_t addrCounter = 0;
    do {
        LPVOID call_via_ptr = (LPVOID)((ULONGLONG)modulePtr + call_via);
        if (call_via_ptr == NULL) break;

        LPVOID thunk_ptr = (LPVOID)((ULONGLONG)modulePtr + thunk_addr);
        if (thunk_ptr == NULL) break;

        DWORD *thunk_val = (DWORD*)thunk_ptr;
        DWORD *call_via_val = (DWORD*)call_via_ptr;
        if (*call_via_val == 0) {
            //nothing to fill, probably the last record
            break;
        }
        ULONGLONG searchedAddr = ULONGLONG(*call_via_val);
        addresses.insert(searchedAddr);
        addrCounter++;
        //---
        call_via += sizeof(DWORD);
        thunk_addr += sizeof(DWORD);
    } while (true);

    return addrCounter;
}

std::string findDllName(std::set<ULONGLONG> &addresses, std::map<ULONGLONG, std::set<ExportedFunc>> &va_to_func)
{
    std::set<std::string> dllNames;
    bool isFresh = true;

    std::set<ULONGLONG>::iterator addrItr;
    for (addrItr = addresses.begin(); addrItr != addresses.end(); addrItr++) {
        ULONGLONG searchedAddr = *addrItr;
        //---
        std::map<ULONGLONG, std::set<ExportedFunc>>::iterator fItr1 = va_to_func.find(searchedAddr);
        
        if (fItr1 != va_to_func.end()) {
            std::set<std::string> currDllNames;

            for (std::set<ExportedFunc>::iterator strItr = fItr1->second.begin(); 
                strItr != fItr1->second.end(); 
                strItr++)
            {
                std::string imp_dll_name = strItr->libName;
                currDllNames.insert(imp_dll_name);
            }

            //printf("> %s\n", strItr->c_str());
            if (!isFresh) {
                std::set<std::string> resultSet;
                std::set_intersection(dllNames.begin(), dllNames.end(),
                    currDllNames.begin(), currDllNames.end(),
                    std::inserter(resultSet, resultSet.begin()));
                dllNames = resultSet;
            } else {
                dllNames = currDllNames;
            }
        }
        //---
    }
    if (dllNames.size() > 0) {
        return *(dllNames.begin());
    }
    return "";
}

size_t mapAddressesToFunctions(std::set<ULONGLONG> &addresses, 
                               std::string coveringDll, 
                               std::map<ULONGLONG, std::set<ExportedFunc>> &va_to_func, 
                               OUT std::map<ULONGLONG, std::set<ExportedFunc, FuncNameLengthCompare>> &addr_to_func
                               )
{
    size_t coveredCount = 0;
    std::set<ULONGLONG>::iterator addrItr;
    for (addrItr = addresses.begin(); addrItr != addresses.end(); addrItr++) {

        ULONGLONG searchedAddr = *addrItr;
        //---
        std::map<ULONGLONG, std::set<ExportedFunc>>::iterator fItr1 = va_to_func.find(searchedAddr);
        
        if (fItr1 != va_to_func.end()) {
            std::set<std::string> currDllNames;

            for (std::set<ExportedFunc>::iterator strItr = fItr1->second.begin(); 
                strItr != fItr1->second.end(); 
                strItr++)
            {
                ExportedFunc func = *strItr;
                std::string dll_name = strItr->libName;
                if (dll_name == coveringDll) {
                    //std::string funcName = strItr->funcName;
                    addr_to_func[searchedAddr].insert(func);
                    coveredCount++;
                }
            }
        }
    }
    return coveredCount;
}

bool fixImports(PVOID modulePtr, size_t moduleSize, std::map<ULONGLONG, std::set<ExportedFunc>> &va_to_func)
{
    bool is64 = is64bit((BYTE*)modulePtr);

    IMAGE_DATA_DIRECTORY *importsDir = get_pe_directory((const BYTE*) modulePtr, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (importsDir == NULL) return false;

    DWORD maxSize = importsDir->Size;
    DWORD impAddr = importsDir->VirtualAddress;

    IMAGE_IMPORT_DESCRIPTOR* lib_desc = NULL;
    DWORD parsedSize = 0;

    printf("---IMP---\n");
    while (parsedSize < maxSize) {

        lib_desc = (IMAGE_IMPORT_DESCRIPTOR*)(impAddr + parsedSize + (ULONG_PTR) modulePtr);
        if (!validate_ptr(modulePtr, moduleSize, lib_desc, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
            printf("[-] Invalid descriptor pointer!\n");
            return false;
        }
        parsedSize += sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (lib_desc->OriginalFirstThunk == NULL && lib_desc->FirstThunk == NULL) {
            break;
        }

        printf("Imported Lib: %x : %x : %x\n", lib_desc->FirstThunk, lib_desc->OriginalFirstThunk, lib_desc->Name);

        LPSTR name_ptr = (LPSTR)((ULONGLONG) modulePtr + lib_desc->Name);
        if (!validate_ptr(modulePtr, moduleSize, name_ptr, sizeof(char) * MIN_DLL_LEN)) {
            printf("[-] Invalid pointer to the name!\n");
            return false;
        }
        std::string lib_name = (LPSTR)((ULONGLONG) modulePtr + lib_desc->Name);
        DWORD call_via = lib_desc->FirstThunk;
        DWORD thunk_addr = lib_desc->OriginalFirstThunk; // warning: it can be NULL!
        std::set<ULONGLONG> addresses;
        if (!is64) {
            findAddressesToFill32(call_via, thunk_addr, modulePtr, addresses);
        } else {
            printf("[-] Support for 64 bit PE is not implemented yet!\n");
            return false;
        }
        if (lib_name.length() == 0) {
            printf("Erased DLL name\n");
            lib_name = findDllName(addresses, va_to_func);
            if (lib_name.length() != 0) {
                std::string found_name = lib_name + ".dll";
                name_ptr = (LPSTR)((ULONGLONG) modulePtr + lib_desc->Name);
                if (!validate_ptr(modulePtr, moduleSize, name_ptr, found_name.length())) {
                    printf("[-] Invalid pointer to the name!\n");
                    return false;
                }
                memcpy(name_ptr, found_name.c_str(), found_name.length());
            }
        } else {
            lib_name = getDllName(lib_name);
        }
        
        if (lib_name.length() == 0) {
            printf("[ERROR] Cannot find DLL!\n");
            return false;
        }
        printf("# %s\n", lib_name.c_str());
        OUT std::map<ULONGLONG, std::set<ExportedFunc, FuncNameLengthCompare>> addr_to_func;
        size_t coveredCount = mapAddressesToFunctions(addresses, lib_name, va_to_func, addr_to_func); 
        if (coveredCount < addresses.size()) {
            printf("[-] Not all addresses are covered! covered: %d total: %d\n", static_cast<int>(coveredCount), static_cast<int>(addresses.size()));
        } else {
            printf("All covered!\n");
        }
        if (!is64) {
            if (!fillImportNames32(lib_desc, modulePtr, moduleSize, addr_to_func)) {
                printf("[-] Could not fill some import names!\n");
                return false;
            }
        } else {
            printf("[-] PE 64-bit is not supported!\n");
        }
    }
    printf("---------\n");
    return true;
}
