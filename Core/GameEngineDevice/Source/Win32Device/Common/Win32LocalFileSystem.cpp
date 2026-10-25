/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

///////// Win32LocalFileSystem.cpp /////////////////////////
// Bryan Cleveland, August 2002
////////////////////////////////////////////////////////////

#include <windows.h>
#include "Common/AsciiString.h"
#include "Common/GameMemory.h"
#include "Common/PerfTimer.h"
#include "Win32Device/Common/Win32LocalFileSystem.h"
#include "Win32Device/Common/Win32LocalFile.h"
#include <io.h>
#include "Common/Registry.h"

namespace
{
// The folder of the installed game that holds the loose "Data" files: the first registered install folder (or the game folder below it)
// that contains Data\Scripts\SkirmishScripts.scb. Empty when there is none.
const AsciiString &installDataRoot()
{
	static AsciiString root;
	static Bool searched = FALSE;
	if (searched)
		return root;
	searched = TRUE;

	AsciiString bases[2];
	GetStringFromRegistry("", "InstallPath", bases[0]);
	GetStringFromGeneralsRegistry("", "InstallPath", bases[1]);
	for (Int i = 0; i < 2 && root.isEmpty(); ++i)
	{
		AsciiString base = bases[i];
		if (base.isEmpty())
			continue;
		const char last = base.getCharAt(base.getLength() - 1);
		if (last != '\\' && last != '/')
			base.concat('\\');

		const char *subFolders[] = { "", "Command and Conquer Generals Zero Hour\\", "ZH_Generals\\" };
		for (const char *sub : subFolders)
		{
			AsciiString candidate = base;
			candidate.concat(sub);
			AsciiString probe = candidate;
			probe.concat("Data\\Scripts\\SkirmishScripts.scb");
			if (_access(probe.str(), 0) == 0)
			{
				root = candidate;
				break;
			}
		}
	}
	return root;
}
}

Bool Win32LocalFileSystem_FindInInstall(const Char *relativeFilename, AsciiString &fullPath)
{
	// only the loose "Data\..." files of the game, never absolute paths or user files
	if (!relativeFilename || _strnicmp(relativeFilename, "data\\", 5) != 0 && _strnicmp(relativeFilename, "data/", 5) != 0)
		return FALSE;
	if (_access(relativeFilename, 0) == 0)
		return FALSE;

	const AsciiString &root = installDataRoot();
	if (root.isEmpty())
		return FALSE;

	AsciiString candidate = root;
	candidate.concat(relativeFilename);
	if (_access(candidate.str(), 0) != 0)
		return FALSE;

	fullPath = candidate;
	return TRUE;
}

Win32LocalFileSystem::Win32LocalFileSystem() : LocalFileSystem()
{
}

Win32LocalFileSystem::~Win32LocalFileSystem() {
}

//DECLARE_PERF_TIMER(Win32LocalFileSystem_openFile)
File * Win32LocalFileSystem::openFile(const Char *filename, Int access, size_t bufferSize)
{
	//USE_PERF_TIMER(Win32LocalFileSystem_openFile)

	// sanity check
	if (*filename == '\0') {
		return nullptr;
	}

	if (access & File::WRITE) {
		// if opening the file for writing, we need to make sure the directory is there
		// before we try to create the file.
		AsciiString string;
		string = filename;
		AsciiString token;
		AsciiString dirName;
		string.nextToken(&token, "\\/");
		dirName = token;
		while (!string.isEmpty()) {
			createDirectory(dirName);
			string.nextToken(&token, "\\/");
			dirName.concat('\\');
			dirName.concat(token);
		}
	}

	// TheSuperHackers @fix Mauller 21/04/2025 Create new file handle when necessary to prevent memory leak
	Win32LocalFile *file = newInstance( Win32LocalFile );

	AsciiString installPath;
	if (!(access & File::WRITE) && Win32LocalFileSystem_FindInInstall(filename, installPath))
		filename = installPath.str();

	if (file->open(filename, access, bufferSize) == FALSE) {
		deleteInstance(file);
		file = nullptr;
	} else {
		file->deleteOnClose();
	}

// this will also need to play nice with the STREAMING type that I added, if we ever enable this

// srj sez: this speeds up INI loading, but makes BIG files unusable.
// don't enable it without further tweaking.
//
// unless you like running really slowly.
//	if (!(access&File::WRITE)) {
//		// Return a ramfile.
//		RAMFile *ramFile = newInstance( RAMFile );
//		if (ramFile->open(file)) {
//			file->close(); // is deleteonclose, so should delete.
//			ramFile->deleteOnClose();
//			return ramFile;
//		}	else {
//			ramFile->close();
//			deleteInstance(ramFile);
//		}
//	}

	return file;
}

void Win32LocalFileSystem::update()
{
}

void Win32LocalFileSystem::init()
{
}

void Win32LocalFileSystem::reset()
{
}

//DECLARE_PERF_TIMER(Win32LocalFileSystem_doesFileExist)
Bool Win32LocalFileSystem::doesFileExist(const Char *filename) const
{
	//USE_PERF_TIMER(Win32LocalFileSystem_doesFileExist)
	if (_access(filename, 0) == 0) {
		return TRUE;
	}

	AsciiString installPath;
	return Win32LocalFileSystem_FindInInstall(filename, installPath);
}

void Win32LocalFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList & filenameList, Bool searchSubdirectories) const
{
	AsciiString directory = originalDirectory;
	directory.concat(currentDirectory);

	AsciiString asciisearch = directory;
	asciisearch.concat(searchName);

	WIN32_FIND_DATA findData;
	HANDLE fileHandle = FindFirstFile(asciisearch.str(), &findData);
	Bool done = (fileHandle == INVALID_HANDLE_VALUE);

	for (; !done; done = (FindNextFile(fileHandle, &findData) == 0))	{
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				(strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)) {
				// if we haven't already, add this filename to the list.
				// a stl set should only allow one copy of each filename
				AsciiString newFilename = directory;
				newFilename.concat(findData.cFileName);
				if (filenameList.find(newFilename) == filenameList.end()) {
					filenameList.insert(newFilename);
				}
		}
	}
	FindClose(fileHandle);

	if (searchSubdirectories) {
		AsciiString subdirsearch = directory;
		subdirsearch.concat("*.");
		fileHandle = FindFirstFile(subdirsearch.str(), &findData);
		done = (fileHandle == INVALID_HANDLE_VALUE);

		for (; !done; done = (FindNextFile(fileHandle, &findData) == 0)) {
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
					(strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)) {

					AsciiString tempsearchstr;
					tempsearchstr.concat(currentDirectory);
					tempsearchstr.concat(findData.cFileName);
					tempsearchstr.concat('\\');

					// recursively add files in subdirectories if required.
					getFileListInDirectory(tempsearchstr, originalDirectory, searchName, filenameList, searchSubdirectories);
			}
		}

		FindClose(fileHandle);
	}

}

Bool Win32LocalFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo) const
{
	WIN32_FIND_DATA findData;
	HANDLE findHandle = nullptr;
	findHandle = FindFirstFile(filename.str(), &findData);

	if (findHandle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	fileInfo->sizeHigh = findData.nFileSizeHigh;
	fileInfo->sizeLow = findData.nFileSizeLow;
	fileInfo->timestampHigh = findData.ftLastWriteTime.dwHighDateTime;
	fileInfo->timestampLow = findData.ftLastWriteTime.dwLowDateTime;

	FindClose(findHandle);

	return TRUE;
}

Bool Win32LocalFileSystem::createDirectory(AsciiString directory)
{
	if ((!directory.isEmpty()) && (directory.getLength() < _MAX_DIR)) {
		return (CreateDirectory(directory.str(), nullptr) != 0);
	}
	return FALSE;
}

AsciiString Win32LocalFileSystem::normalizePath(const AsciiString& filePath) const
{
	DWORD retval = GetFullPathNameA(filePath.str(), 0, nullptr, nullptr);
	if (retval == 0)
	{
		DEBUG_LOG(("Unable to determine buffer size for normalized file path. Error=(%u).", GetLastError()));
		return AsciiString::TheEmptyString;
	}

	AsciiString normalizedFilePath;
	retval = GetFullPathNameA(filePath.str(), retval, normalizedFilePath.getBufferForRead(retval - 1), nullptr);
	if (retval == 0)
	{
		DEBUG_LOG(("Unable to normalize file path '%s'. Error=(%u).", filePath.str(), GetLastError()));
		return AsciiString::TheEmptyString;
	}

	return normalizedFilePath;
}
