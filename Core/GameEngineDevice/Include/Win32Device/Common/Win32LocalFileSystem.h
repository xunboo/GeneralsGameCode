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

///// Win32LocalFileSystem.h //////////////////////////////////
// Bryan Cleveland, August 2002
///////////////////////////////////////////////////////////////

#pragma once

#include "Common/LocalFileSystem.h"

/**
 * Loose game files (Data\Scripts\SkirmishScripts.scb, Data\Cursors, ...) are looked up relative to the working directory. When the game is started
 * from another folder (for example the build output), they are looked up in the installed game next to the registered .big archives instead.
 * Returns true and the full path when a relative file is missing here but exists in the installation.
 */
Bool Win32LocalFileSystem_FindInInstall(const Char *relativeFilename, AsciiString &fullPath);

class Win32LocalFileSystem : public LocalFileSystem
{
public:
	Win32LocalFileSystem();
	virtual ~Win32LocalFileSystem() override;

	virtual void init() override;
	virtual void reset() override;
	virtual void update() override;

	virtual File * openFile(const Char *filename, Int access = File::NONE, size_t bufferSize = File::BUFFERSIZE) override;	///< open the given file.
	virtual Bool doesFileExist(const Char *filename) const override;								///< does the given file exist?

	virtual void getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const override; ///< search the given directory for files matching the searchName (egs. *.ini, *.rep).  Possibly search subdirectories.
	virtual Bool getFileInfo(const AsciiString& filename, FileInfo *fileInfo) const override;

	virtual Bool createDirectory(AsciiString directory) override;
	virtual AsciiString normalizePath(const AsciiString& filePath) const override;

protected:
};
