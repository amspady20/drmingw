/* symbols.c
 *
 *
 * Jose Fonseca <j_r_fonseca@yahoo.co.uk>
 */

#include <assert.h>

#include <windows.h>
#include <tchar.h>
#include <stdlib.h>
#include <psapi.h>

#include "misc.h"
#include "symbols.h"
#include "bfdhelp.h"


// The GetModuleBase function retrieves the base address of the module that contains the specified address. 
DWORD64 GetModuleBase(HANDLE hProcess, DWORD64 dwAddress)
{
	MEMORY_BASIC_INFORMATION Buffer;
	
	return VirtualQueryEx(hProcess, (LPCVOID) dwAddress, &Buffer, sizeof(Buffer)) ? (DWORD64) Buffer.AllocationBase : 0;
}


#ifdef HEADER
#include <dbghelp.h>
#endif /* HEADER */

BOOL bSymInitialized = FALSE;

BOOL GetSymFromAddr(HANDLE hProcess, DWORD64 dwAddress, LPTSTR lpSymName, DWORD nSize)
{
	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)malloc(sizeof(SYMBOL_INFO) + nSize * sizeof(TCHAR));

	DWORD64 dwDisplacement = 0;  // Displacement of the input address, relative to the start of the symbol
	BOOL bRet;

	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = nSize;

	assert(bSymInitialized);
	
	bRet = BfdSymFromAddr(hProcess, dwAddress, &dwDisplacement, pSymbol);

	if (bRet) {
		lstrcpyn(lpSymName, pSymbol->Name, nSize);
	}

	free(pSymbol);

	return bRet;
}

BOOL GetLineFromAddr(HANDLE hProcess, DWORD64 dwAddress,  LPTSTR lpFileName, DWORD nSize, LPDWORD lpLineNumber)
{
	IMAGEHLP_LINE64 Line;
	DWORD dwDisplacement = 0;  // Displacement of the input address, relative to the start of the symbol

	// Do the source and line lookup.
	memset(&Line, 0, sizeof Line);
	Line.SizeOfStruct = sizeof Line;
	
	assert(bSymInitialized);

#if 0
	{
		// The problem is that the symbol engine only finds those source
		//  line addresses (after the first lookup) that fall exactly on
		//  a zero displacement.  I will walk backwards 100 bytes to
		//  find the line and return the proper displacement.
		DWORD64 dwTempDisp = 0 ;
		while (dwTempDisp < 100 && !BfdSymGetLineFromAddr64(hProcess, dwAddress - dwTempDisp, &dwDisplacement, &Line))
			++dwTempDisp;
		
		if(dwTempDisp >= 100)
			return FALSE;
			
		// It was found and the source line information is correct so
		//  change the displacement if it was looked up multiple times.
		if (dwTempDisp < 100 && dwTempDisp != 0 )
			dwDisplacement = dwTempDisp;
	}
#else
	if(!BfdSymGetLineFromAddr64(hProcess, dwAddress, &dwDisplacement, &Line))
		return FALSE;
#endif

	assert(lpFileName && lpLineNumber);
	
	lstrcpyn(lpFileName, Line.FileName, nSize);
	*lpLineNumber = Line.LineNumber;
	
	return TRUE;
}

static PIMAGE_NT_HEADERS
PEImageNtHeader(HANDLE hProcess, HMODULE hModule)
{
	PIMAGE_DOS_HEADER pDosHeader;
	PIMAGE_NT_HEADERS pNtHeaders;
	LONG e_lfanew;
	
	// Point to the DOS header in memory
	pDosHeader = (PIMAGE_DOS_HEADER)hModule;
	
	// From the DOS header, find the NT (PE) header
	if(!ReadProcessMemory(hProcess, &pDosHeader->e_lfanew, &e_lfanew, sizeof(e_lfanew), NULL))
		return NULL;
	
	pNtHeaders = (PIMAGE_NT_HEADERS)((LPBYTE)hModule + e_lfanew);

	return pNtHeaders;
}

BOOL PEGetSymFromAddr(HANDLE hProcess, DWORD64 dwAddress, LPTSTR lpSymName, DWORD nSize)
{
	HMODULE hModule;
	PIMAGE_NT_HEADERS pNtHeaders;
	IMAGE_NT_HEADERS NtHeaders;
	PIMAGE_SECTION_HEADER pSection;
	DWORD64 dwNearestAddress = 0;
	DWORD dwNearestName;
	int i;

	if(!(hModule = (HMODULE) GetModuleBase(hProcess, dwAddress)))
		return FALSE;
	
	if(!(pNtHeaders = PEImageNtHeader(hProcess, hModule)))
		return FALSE;

	if(!ReadProcessMemory(hProcess, pNtHeaders, &NtHeaders, sizeof(IMAGE_NT_HEADERS), NULL))
		return FALSE;
	
	pSection = (PIMAGE_SECTION_HEADER) ((DWORD64)pNtHeaders + sizeof(DWORD64) + sizeof(IMAGE_FILE_HEADER) + NtHeaders.FileHeader.SizeOfOptionalHeader);

	if (0)
		OutputDebug("Exported symbols:\r\n");

	// Look for export section
	for (i = 0; i < NtHeaders.FileHeader.NumberOfSections; i++, pSection++)
	{
		IMAGE_SECTION_HEADER Section;
		DWORD64 pExportDir = 0;
		BYTE ExportSectionName[IMAGE_SIZEOF_SHORT_NAME] = {'.', 'e', 'd', 'a', 't', 'a', '\0', '\0'};
		
		if(!ReadProcessMemory(hProcess, pSection, &Section, sizeof Section, NULL))
			return FALSE;
    	
		if(memcmp(Section.Name, ExportSectionName, IMAGE_SIZEOF_SHORT_NAME) == 0)
			pExportDir = Section.VirtualAddress;
		else if ((NtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress >= Section.VirtualAddress) && (NtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress < (Section.VirtualAddress + Section.SizeOfRawData)))
			pExportDir = NtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;

		if(pExportDir)
		{
			IMAGE_EXPORT_DIRECTORY ExportDir;
			
			if(!ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + (DWORD64)pExportDir), &ExportDir, sizeof ExportDir, NULL))
				return FALSE;
			
			{
				PDWORD *AddressOfFunctions = alloca(ExportDir.NumberOfFunctions*sizeof(PDWORD));
				int j;
	
				if(!ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + (DWORD64)ExportDir.AddressOfFunctions), AddressOfFunctions, ExportDir.NumberOfFunctions*sizeof(PDWORD), NULL))
						return FALSE;
				
				for(j = 0; j < ExportDir.NumberOfNames; ++j)
				{
					DWORD64 pFunction = (DWORD64)hModule + (DWORD64)AddressOfFunctions[j];
					
					if(pFunction <= dwAddress && pFunction > dwNearestAddress)
					{
						dwNearestAddress = pFunction;
						
						if(!ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + ExportDir.AddressOfNames + j*sizeof(DWORD)), &dwNearestName, sizeof(dwNearestName), NULL))
							return FALSE;
					}
				
					if (0)
					{
						DWORD dwName;
						char szName[256];
						
						if(ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + ExportDir.AddressOfNames * j*sizeof(DWORD)), &dwName, sizeof(dwName), NULL))
							if(ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + dwName), szName, sizeof(szName), NULL))
								OutputDebug("\t%08lx\t%s\r\n", pFunction, szName);
					}
				}
			}
		}		
    }

	if(!dwNearestAddress)
		return FALSE;
		
	if(!ReadProcessMemory(hProcess, (PVOID)((DWORD64)hModule + dwNearestName), lpSymName, nSize, NULL))
		return FALSE;
	lpSymName[nSize - 1] = 0;

	return TRUE;
}
