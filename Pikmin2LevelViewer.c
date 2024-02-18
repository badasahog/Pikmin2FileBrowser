/*
* (C) 2024 badasahog. All Rights Reserved
* The above copyright notice shall be included in
* all copies or substantial portions of the Software.
*/
#include <Windows.h>
#include <commctrl.h>

#pragma comment(linker, "/DEFAULTLIB:comctl32.lib")

#include <stdio.h>
#include <stdint.h>

//c23 compatibility stuff:
#include <stdbool.h>
#include <stdalign.h>
#include <assert.h>

#define nullptr ((void*)0)

HANDLE ConsoleHandle;

inline void THROW_ON_FAIL_IMPL(HRESULT hr, int line)
{
	if (FAILED(hr))
	{
		LPWSTR messageBuffer;
		DWORD formattedErrorLength = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			hr,
			MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			(LPWSTR)&messageBuffer,
			0,
			nullptr
		);

		if (formattedErrorLength == 0)
			WriteConsoleA(ConsoleHandle, "an error occured, unable to retrieve error message\n", 51, nullptr, nullptr);
		else
		{
			WriteConsoleA(ConsoleHandle, "an error occured: ", 18, nullptr, nullptr);
			WriteConsoleW(ConsoleHandle, messageBuffer, formattedErrorLength, nullptr, nullptr);
			WriteConsoleA(ConsoleHandle, "\n", 1, nullptr, nullptr);
		}

		char buffer[50];
		int stringlength = _snprintf_s(buffer, 50, _TRUNCATE, "error code: 0x%X\nlocation:line %i\n", hr, line);
		WriteConsoleA(ConsoleHandle, buffer, stringlength, nullptr, nullptr);



		RaiseException(0, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}
}

#define THROW_ON_FAIL(x) THROW_ON_FAIL_IMPL(x, __LINE__)

#define THROW_ON_FALSE(x) if((x) == FALSE) THROW_ON_FAIL(GetLastError())

#define VALIDATE_HANDLE(x) if((x) == nullptr || (x) == INVALID_HANDLE_VALUE) THROW_ON_FAIL(GetLastError())

#define DEBUG_ASSERT(x) if(((bool)(x)) != true && IsDebuggerPresent() == TRUE)DebugBreak();

#define SwapEndian(x) (typeof(x))_Generic((x), \
						short: _byteswap_ushort,\
						unsigned short: _byteswap_ushort,\
						int: _byteswap_ulong,  \
						unsigned int: _byteswap_ulong, \
						long: _byteswap_ulong, \
						unsigned long: _byteswap_ulong, \
						long long: _byteswap_uint64, \
						unsigned long long: _byteswap_uint64 \
						)(x)

#define SwapEndianFloat(x) IntAsFloat(_byteswap_ulong(FloatAsInt(x)))

#define OffsetPointer(x, offset) ((typeof(x))((char*)x + (offset)))

#define countof(x) (sizeof(x) / sizeof(x[0]))

void* GameImageAddress;


struct gameFileInTree
{
	HTREEITEM treeItem;
	void* filePtr;
	uint32_t fileSize;
};


struct gameFileInTree* gameFileList;

/*
* file types:
* 0: none
* 1: *.txt
* 2: *.ini
*/
static int displayedFileType = 0;

static int selectedFileIndex = 0;

LRESULT CALLBACK FileViewerWindowProcedure(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static SCROLLINFO si = { 0 };
	switch (msg)
	{
		case WM_CREATE:
		{
			si.cbSize = sizeof(si);
			si.fMask = SIF_RANGE | SIF_PAGE;
			si.nMin = 0;
			si.nMax = 200;
			si.nPage = 50;
			SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
			break;
		}
		case WM_VSCROLL:
		{
			si.cbSize = sizeof(si);
			si.fMask = SIF_ALL;
			GetScrollInfo(hwnd, SB_VERT, &si);
			switch (LOWORD(wParam))
			{
			case SB_TOP:
				si.nPos = si.nMin;
				break;
			case SB_BOTTOM:
				si.nPos = si.nMax;
				break;
			case SB_LINEUP:
				si.nPos -= 1;
				break;
			case SB_LINEDOWN:
				si.nPos += 1;
				break;
			case SB_PAGEUP:
				si.nPos -= si.nPage;
				break;
			case SB_PAGEDOWN:
				si.nPos += si.nPage;
				break;
			case SB_THUMBTRACK:
				si.nPos = si.nTrackPos;
				break;
			default:
				break;
			}
			si.fMask = SIF_POS;
			SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

			InvalidateRect(hwnd, nullptr, TRUE);
			UpdateWindow(hwnd);
		}
		break;
		case WM_PAINT:
		{
			if (displayedFileType == 1 || displayedFileType == 2)
			{
				PAINTSTRUCT ps = { 0 };
				HDC hdc = BeginPaint(hwnd, &ps);
				RECT rect = { 25, si.nPos * -25, 500, 2000 };
				DrawTextA(hdc, gameFileList[selectedFileIndex].filePtr, -1, &rect, DT_LEFT | DT_TOP);
				EndPaint(hwnd, &ps);
			}
			break;
		}
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}

LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static HWND hTreeView;
	static HWND hFileView;

	switch (msg)
	{
	case WM_CREATE:
	{
		WNDCLASSW wc = { 0 };

		wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"FileViewerWindowClass";
		wc.lpfnWndProc = &FileViewerWindowProcedure;
		RegisterClassW(&wc);


		hTreeView = CreateWindowExW(0, WC_TREEVIEW, L"Tree View",
			WS_VISIBLE | WS_CHILD | WS_BORDER | TVS_HASLINES,
			0, 0, 200, 500,
			hwnd, NULL, NULL, NULL);

		hFileView = CreateWindowExW(0, L"FileViewerWindowClass", L"File View",
			WS_VISIBLE | WS_CHILD | WS_BORDER | TVS_HASLINES | WS_VSCROLL,
			200, 0, 200, 500,
			hwnd, NULL, NULL, NULL);

		{
			struct DiskHeader
			{
				uint32_t GameCode;
				uint16_t MakerCode;
				uint8_t DiskID;
				uint8_t Version;
				uint8_t AudioStreaming;
				uint8_t StreamBufferSize;
				uint8_t unused1[18];
				uint32_t Magic;
				char GameName[992];
				uint32_t DebugMonitorOffset;
				uint32_t Unknown;
				uint8_t unused2[24];
				uint32_t DOLOffset;
				uint32_t FSTOffset;
				uint32_t FSTSize;
				uint32_t MaxFSTSize;
			};

			struct DiskHeader* dh = GameImageAddress;

			struct FileEntry
			{
				uint8_t Flags;
				uint8_t FileNameOffsetp1;
				uint8_t FileNameOffsetp2;
				uint8_t FileNameOffsetp3;
				uint32_t FileOffset;
				uint32_t Unknown;
			};

			struct FileEntry* FST = OffsetPointer(GameImageAddress, SwapEndian(dh->FSTOffset));
			void* StringTable = OffsetPointer(FST, SwapEndian(FST->Unknown) * sizeof(struct FileEntry));
			int NumEntries = SwapEndian(FST->Unknown);

			//how many directories in we are (max 16)
			int deepness = 0;

			uint32_t endOfDirectoryLocation[16] = { 0 };
			HTREEITEM parentDirectoryDropdowns[16] = { 0 };

			TVINSERTSTRUCTW rootNode = { 0 };
			rootNode.hParent = NULL;
			rootNode.hInsertAfter = TVI_ROOT;
			rootNode.item.mask = TVIF_TEXT;
			rootNode.item.pszText = L"<root>";

			parentDirectoryDropdowns[0] = (HTREEITEM)SendMessageW(hTreeView, TVM_INSERTITEMW, 0, (LPARAM)&rootNode);

			int x = 0;

			for (int i = 0; i < NumEntries; i++)
			{
				struct FileEntry* FE = FST + i;

				uint32_t FileNameOffset = (FE->FileNameOffsetp1 << 16) | (FE->FileNameOffsetp2 << 8) | FE->FileNameOffsetp3;

				if (i != 0)
				{
					if (FE->Flags == 1)//directories
					{
						deepness++;
						endOfDirectoryLocation[deepness] = i;

						wchar_t itemname[100];

						MultiByteToWideChar(
							CP_OEMCP,
							0,
							OffsetPointer(StringTable, FileNameOffset),
							-1,
							itemname,
							100
							);

						rootNode.hParent = parentDirectoryDropdowns[deepness - 1];
						rootNode.item.pszText = itemname;


						parentDirectoryDropdowns[deepness] = (HTREEITEM)SendMessageW(hTreeView, TVM_INSERTITEMW, 0, (LPARAM)&rootNode);

					}
					else//files
					{
						wchar_t itemname[100];

						MultiByteToWideChar(
							CP_OEMCP,
							0,
							OffsetPointer(StringTable, FileNameOffset),
							-1,
							itemname,
							100
						);

						rootNode.hParent = parentDirectoryDropdowns[deepness];
						rootNode.item.mask = TVIF_TEXT | TVIF_PARAM;
						rootNode.item.pszText = itemname;
						rootNode.item.lParam = (LPARAM)x;

						gameFileList[x].treeItem = (HTREEITEM)SendMessageW(hTreeView, TVM_INSERTITEMW, 0, (LPARAM)&rootNode);

						gameFileList[x].filePtr = OffsetPointer(GameImageAddress, SwapEndian(FE->FileOffset));

						gameFileList[x].fileSize = SwapEndian(FE->Unknown);

						x++;

						while (i >= (SwapEndian((FST + endOfDirectoryLocation[deepness])->Unknown) - 1))
						{
							deepness--;
						}
					}
				}
			}
		}
		break;
	}
	case WM_SIZE:
	{
		MoveWindow(hTreeView, 0, 0, LOWORD(lParam) / 5, HIWORD(lParam), TRUE);
		MoveWindow(hFileView, LOWORD(lParam) / 5, 0, LOWORD(lParam) - (LOWORD(lParam) / 5), HIWORD(lParam), TRUE);
		InvalidateRect(hFileView, nullptr, TRUE);
		UpdateWindow(hFileView);

		break;

	}
	case WM_NOTIFY:
	{
		LPNMHDR lpnmh = (LPNMHDR)lParam;
		if (lpnmh->code == TVN_SELCHANGED)
		{
			//printf("selection changed ");


			LPNMTREEVIEWW lpnm = (LPNMTREEVIEWW)lParam;

			HWND hTreeView = lpnm->hdr.hwndFrom;
			HTREEITEM hSelected = lpnm->itemNew.hItem;

			TCHAR buffer[64];
			TVITEMW tvi;

			tvi.mask = TVIF_TEXT;
			tvi.hItem = hSelected;
			tvi.pszText = buffer;
			tvi.cchTextMax = sizeof(buffer) / sizeof(buffer[0]);

			SendMessageW(hTreeView, TVM_GETITEM, 0, (LPARAM)&tvi);

			//wprintf(L"selected item: %s\n", tvi.pszText);

			const wchar_t* extensionType = wcsrchr(tvi.pszText, L'.');

			for (int i = 0; i < 5000; i++)
			{
				if (gameFileList[i].treeItem == hSelected)
				{
					selectedFileIndex = i;

					if (wcscmp(extensionType, L".szs") == 0)
					{
						printf("dealing with a compressed szs file!\n");

						struct yaz0Header
						{
							char magic[4];
							uint32_t uncompressedSize;
							uint32_t reserved1;
							uint32_t reserved2;
						};


						const struct yaz0Header* header = gameFileList[selectedFileIndex].filePtr;

						printf("magic: ");
						WriteConsoleA(ConsoleHandle, &header->magic, 4, nullptr, nullptr);
						printf("\nuncompressed size: %i\n", SwapEndian(header->uncompressedSize));



						const uint8_t* src = OffsetPointer(gameFileList[selectedFileIndex].filePtr, sizeof(struct yaz0Header));// pointer to start of source
						const uint8_t* src_end = OffsetPointer(src, gameFileList[selectedFileIndex].fileSize - sizeof(struct yaz0Header));// pointer to end of source (last byte +1)
						//todo: malloc is temporary
						uint8_t* dest = malloc(SwapEndian(header->uncompressedSize));// pointer to start of destination
						memset(dest, 0, SwapEndian(header->uncompressedSize));
						uint8_t* dest_end = OffsetPointer(dest, SwapEndian(header->uncompressedSize));// pointer to end of destination (last byte +1)

						uint8_t  group_head = 0; // group header byte ...
						int group_head_len = 0; // ... and it's length to manage groups

						while (src < src_end && dest < dest_end)
						{
							if (!group_head_len)
							{
								//*** start a new data group and read the group header byte.

								group_head = *src++;
								group_head_len = 8;
							}

							group_head_len--;
							if (group_head & 0x80)
							{
								//*** bit in group header byte is set -> copy 1 byte direct

								*dest++ = *src++;
							}
							else
							{
								//*** bit in group header byte is not set -> run length encoding

							// read the first 2 bytes of the chunk
								const uint8_t b1 = *src++;
								const uint8_t b2 = *src++;

								// calculate the source position
								const uint8_t* copy_src = dest - ((b1 & 0x0f) << 8 | b2) - 1;

								// calculate the number of bytes to copy.
								int n = b1 >> 4;

								if (!n)
									n = *src++ + 0x12; // N==0 -> read third byte
								else
									n += 2; // add 2 to length
								assert(n >= 3 && n <= 0x111);

								// a validity check
								// todo: figure out what this was for
								if (/*copy_src < szs->data || */dest + n > dest_end)
									printf("critical error!\n");

								// copy chunk data.
									// don't use memcpy() or memmove() here because
									// they don't work with self referencing chunks.
								while (n-- > 0)
									*dest++ = *copy_src++;
							}

							// shift group header byte
							group_head <<= 1;
						}

						// some assertions to find errors in debugging mode
						assert(src <= src_end);
						assert(dest <= dest_end);

						//todo: this is printing out shit that doesn't make sense for an uncompressed szs
						for (int j = 0; j < 32; j++)
						{
							printf("%c", dest[j]);
						}
						printf("\n");

					}
					else if (wcscmp(extensionType, L".txt") == 0)
					{
						displayedFileType = 1;
					}
					else if (wcscmp(extensionType, L".ini") == 0)
					{
						displayedFileType = 2;
					}
					else
					{
						displayedFileType = 0;
					}
					break;
				}
			}

			InvalidateRect(hFileView, nullptr, TRUE);
			UpdateWindow(hFileView);
		}
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}


int main()
{
	ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	//todo: temporary!
	gameFileList = malloc(1024 * 1024 * 24);

	OPENFILENAME ofn = { 0 };
	WCHAR szFile[260] = { 0 };

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = nullptr;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = L"Pikmin 2\0*.iso\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = nullptr;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = nullptr;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	THROW_ON_FALSE(GetOpenFileNameW(&ofn));

	HANDLE file = CreateFileW(
		szFile,
		GENERIC_READ,
		0,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	VALIDATE_HANDLE(file);

	HANDLE hMapFile = CreateFileMappingW(
		file,
		nullptr,
		PAGE_READONLY,
		0,
		0,
		nullptr);
	VALIDATE_HANDLE(hMapFile);

	//todo: __assume is a compiler extension
	__assume(hMapFile != 0);

	GameImageAddress = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);
	__assume(GameImageAddress != nullptr);


	WNDCLASSW wc = { 0 };

	wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"myWindowClass";
	wc.lpfnWndProc = WindowProcedure;

	if (!RegisterClassW(&wc))
		return -1;

	CreateWindowExW(0, L"myWindowClass", L"Pikmin Asset Viewer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		100, 100, 500, 500, NULL, NULL, NULL, NULL);

	MSG msg = { 0 };

	while (GetMessageW(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}
