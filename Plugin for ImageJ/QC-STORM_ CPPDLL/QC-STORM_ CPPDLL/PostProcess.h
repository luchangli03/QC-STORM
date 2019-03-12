/*
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU LESSER GENERAL PUBLIC LICENSE as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU LESSER GENERAL PUBLIC LICENSE for more details.

You should have received a copy of the GNU LESSER GENERAL PUBLIC LICENSE
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "OnlineLocalizationLD.h"


extern CString RerendDataPath;

extern CWinThread *Thread_Rerend;
extern CWinThread *Thread_ConvertBin;

extern volatile int RerendProgress;

extern int IsDriftCorrection;
extern int DriftCorrGroupFrameNum;


UINT th_RerendImage(LPVOID params);

