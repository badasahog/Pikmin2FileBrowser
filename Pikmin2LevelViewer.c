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

struct decodedAsset
{
	uint32_t assetType;
	void* assetPtr;
};
const uint32_t ASSET_TYPE_TEXT = 0;
const uint32_t ASSET_TYPE_TEXTURE = 1;

struct decodedAsset* decodedAssetTable;
int decodedAssetCount = 0;
void* decodedAssetData;

struct decodedImage
{
	uint32_t width;
	uint32_t height;
	uint32_t pixelCount;
	const void* pixels;
};

static int displayedFileType = 0;

static int selectedFileIndex = 0;


int Unpack565(const uint8_t* const _In_ packed, uint8_t* _Out_ color)
{
	// build the packed value - GCN: indices reversed
	const int value = (int)packed[1] | ((int)packed[0] << 8);

	// get the components in the stored range
	const uint8_t red = (uint8_t)((value >> 11) & 0x1f);
	const uint8_t green = (uint8_t)((value >> 5) & 0x3f);
	const uint8_t blue = (uint8_t)(value & 0x1f);

	// scale up to 8 bits
	color[0] = (red << 3) | (red >> 2);
	color[1] = (green << 2) | (green >> 4);
	color[2] = (blue << 3) | (blue >> 2);
	color[3] = 255;


	return value;
}

void DecompressColorGCN(const uint32_t _In_ texWidth, uint8_t* _Out_writes_bytes_all_(texWidth * 32) rgba, const void* const _In_ block)
{
	// get the block bytes
	const uint8_t* bytes = block;

	// unpack the endpoints
	uint8_t codes[16];
	const int a = Unpack565(bytes, codes);
	const int b = Unpack565(bytes + 2, codes + 4);

	// generate the midpoints
	for (int i = 0; i < 3; ++i)
	{
		const int c = codes[i];
		const int d = codes[4 + i];

		if (a <= b)
		{
			codes[8 + i] = (uint8_t)((c + d) / 2);
			// GCN: Use midpoint RGB rather than black
			codes[12 + i] = codes[8 + i];
		}
		else
		{
			// GCN: 3/8 blend rather than 1/3
			codes[8 + i] = (uint8_t)((c * 5 + d * 3) >> 3);
			codes[12 + i] = (uint8_t)((c * 3 + d * 5) >> 3);
		}
	}

	// fill in alpha for the intermediate values
	codes[8 + 3] = 255;
	codes[12 + 3] = (a <= b) ? 0 : 255;

	// unpack the indices
	uint8_t indices[16];
	for (int i = 0; i < 4; ++i)
	{
		uint8_t* ind = indices + 4 * i;
		uint8_t packed = bytes[4 + i];

		// GCN: indices reversed
		ind[3] = packed & 0x3;
		ind[2] = (packed >> 2) & 0x3;
		ind[1] = (packed >> 4) & 0x3;
		ind[0] = (packed >> 6) & 0x3;
	}

	// store out the colors
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
		{
			uint8_t offset = 4 * indices[y * 4 + x];
			for (int j = 0; j < 4; j++)
			{
				rgba[4 * ((y * texWidth + x)) + (j)] = codes[offset + j];//+ (i - 8 < 0 ? 0 : 128)
			}// - i % 4
		}
}

void decodeTexture(uint32_t width, uint32_t height, uint32_t pixelCount, const uint8_t* _In_ pixelsIn, uint8_t* _Out_ pixelsOut, const uint8_t format)
{
	switch (format)
	{
	case 0xE://cmpr
	{

		uint32_t ByteInBlock = 0;

		for (int y = 0; y < height; y += 8)
		{
			for (int x = 0; x < width; x += 8)
			{
				//decode full dxt1 block, (4 sub blocks)
				DecompressColorGCN(width, OffsetPointer(pixelsOut, 4 * (y * width + x)), &pixelsIn[ByteInBlock]);
				ByteInBlock += 8;
				DecompressColorGCN(width, OffsetPointer(pixelsOut, 4 * ((y)*width + (x + 4))), &pixelsIn[ByteInBlock]);
				ByteInBlock += 8;
				DecompressColorGCN(width, OffsetPointer(pixelsOut, 4 * ((y + 4) * width + (x))), &pixelsIn[ByteInBlock]);
				ByteInBlock += 8;
				DecompressColorGCN(width, OffsetPointer(pixelsOut, 4 * ((y + 4) * width + (x + 4))), &pixelsIn[ByteInBlock]);
				ByteInBlock += 8;
			}
		}
		break;
	}
	case 0x5://RGB5A3
	{
		int inputPixelIndex = 0;
		for (int y = 0; y < height; y += 4)
		{
			for (int x = 0; x < width; x += 4)
			{
				for (int j = 0; j < 4; j++)
				{
					for (int k = 0; k < 4; k++)
					{
						uint16_t pixel = SwapEndian(*OffsetPointer((uint16_t*)pixelsIn, inputPixelIndex * sizeof(uint16_t)));

						uint32_t outputPixel = 0;
						if (pixel & 0b1000000000000000)
						{
							//no alpha
							uint8_t pixelB = ((pixel & 0b0000000000011111) >> 0) * 0x8;
							uint8_t pixelG = ((pixel & 0b0000001111100000) >> 5) * 0x8;
							uint8_t pixelR = ((pixel & 0b0111110000000000) >> 10) * 0x8;

							*OffsetPointer((uint8_t*)&outputPixel, 0) = pixelR;
							*OffsetPointer((uint8_t*)&outputPixel, 1) = pixelG;
							*OffsetPointer((uint8_t*)&outputPixel, 2) = pixelB;
							*OffsetPointer((uint8_t*)&outputPixel, 3) = 255;
						}
						else
						{
							//3 bit alpha
							uint8_t pixelB = ((pixel & 0b0000000000001111) >> 0) * 0x11;
							uint8_t pixelG = ((pixel & 0b0000000011110000) >> 4) * 0x11;
							uint8_t pixelR = ((pixel & 0b0000111100000000) >> 8) * 0x11;
							uint8_t pixelA = ((pixel & 0b0111000000000000) >> 12) * 0x20;

							*OffsetPointer((uint8_t*)&outputPixel, 0) = pixelR;
							*OffsetPointer((uint8_t*)&outputPixel, 1) = pixelG;
							*OffsetPointer((uint8_t*)&outputPixel, 2) = pixelB;
							*OffsetPointer((uint8_t*)&outputPixel, 3) = pixelA;
						}
						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k))) = outputPixel;
						inputPixelIndex++;
					}
				}
			}
		}
		break;
	}
	case 0x3://IA8
	{
		int inputPixelIndex = 0;
		for (int y = 0; y < height; y += 4)
		{
			for (int x = 0; x < width; x += 4)
			{
				for (int j = 0; j < 4; j++)
				{
					for (int k = 0; k < 4; k++)
					{
						uint16_t pixel = SwapEndian(*OffsetPointer((uint16_t*)pixelsIn, inputPixelIndex * sizeof(uint16_t)));

						uint32_t outputPixel = 0;


						*OffsetPointer((uint8_t*)&outputPixel, 0) = pixel & 0xFF;
						*OffsetPointer((uint8_t*)&outputPixel, 1) = pixel & 0xFF;
						*OffsetPointer((uint8_t*)&outputPixel, 2) = pixel & 0xFF;
						*OffsetPointer((uint8_t*)&outputPixel, 3) = (pixel & 0xFF00) >> 8;

						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k))) = outputPixel;
						inputPixelIndex++;
					}
				}
			}
		}
		break;
	}
	case 0x0://I4 //todo: untested
	{
		int inputPixelIndex = 0;
		for (int y = 0; y < height; y += 8)
		{
			for (int x = 0; x < width; x += 8)
			{
				for (int j = 0; j < 8; j++)
				{
					for (int k = 0; k < 8; k += 2)
					{
						uint8_t pixel = *OffsetPointer((uint8_t*)pixelsIn, inputPixelIndex * sizeof(uint8_t));

						uint32_t outputPixel = 0;

						*OffsetPointer((uint8_t*)&outputPixel, 0) = (pixel & 0b11110000) >> 4;
						*OffsetPointer((uint8_t*)&outputPixel, 1) = (pixel & 0b11110000) >> 4;
						*OffsetPointer((uint8_t*)&outputPixel, 2) = (pixel & 0b11110000) >> 4;
						*OffsetPointer((uint8_t*)&outputPixel, 3) = 0xFF;

						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k))) = outputPixel;

						*OffsetPointer((uint8_t*)&outputPixel, 0) = (pixel & 0b00001111);
						*OffsetPointer((uint8_t*)&outputPixel, 1) = (pixel & 0b00001111);
						*OffsetPointer((uint8_t*)&outputPixel, 2) = (pixel & 0b00001111);
						*OffsetPointer((uint8_t*)&outputPixel, 3) = 0xFF;


						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k + 1))) = outputPixel;
						inputPixelIndex++;
					}
				}
			}
		}
		break;
	}
	case 0x2://IA4 //todo: untested
	{
		int inputPixelIndex = 0;
		for (int y = 0; y < height; y += 4)
		{
			for (int x = 0; x < width; x += 8)
			{
				for (int j = 0; j < 4; j++)
				{
					for (int k = 0; k < 8; k++)
					{
						uint8_t pixel = *OffsetPointer((uint8_t*)pixelsIn, inputPixelIndex * sizeof(uint8_t));

						uint32_t outputPixel = 0;

						uint8_t grayScale = (pixel & 0b00001111) * 0x11;
						uint8_t alpha = ((pixel & 0b11110000) >> 4) * 0x11;

						*OffsetPointer((uint8_t*)&outputPixel, 0) = grayScale;
						*OffsetPointer((uint8_t*)&outputPixel, 1) = grayScale;
						*OffsetPointer((uint8_t*)&outputPixel, 2) = grayScale;
						*OffsetPointer((uint8_t*)&outputPixel, 3) = alpha;

						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k))) = outputPixel;
						inputPixelIndex++;
					}
				}
			}
		}
		break;
	}
	case 0x4://RGB565
	{
		int inputPixelIndex = 0;
		for (int y = 0; y < height; y += 4)
		{
			for (int x = 0; x < width; x += 4)
			{
				for (int j = 0; j < 4; j++)
				{
					for (int k = 0; k < 4; k++)
					{
						//Unpack565() swaps endian internally
						uint16_t pixel = *OffsetPointer((uint16_t*)pixelsIn, inputPixelIndex * sizeof(uint16_t));

						uint32_t outputPixel = 0;
						Unpack565(&pixel, &outputPixel);


						*((uint32_t*)OffsetPointer(pixelsOut, 4 * (((y + j) * width + x) + k))) = outputPixel;
						inputPixelIndex++;
					}
				}
			}
		}
		break;
	}
	default:

		//DebugBreak();
	}
}

int DecompressYAZ
(
	// returns:
	//	    ERR_OK:           compression done
	//	    ERR_WARNING:      silent==true: dest buffer too small
	//	    ERR_INVALID_DATA: invalid source data

	const void* data,		// source data
	size_t		data_size,	// size of 'data'
	void* dest_buf,	// destination buffer (decompressed data)
	size_t		dest_buf_size,	// size of 'dest_buf'
	size_t* write_status,	// number of written bytes
	//ccp			fname,		// file name for error messages
	int			yaz_version,	// yaz version for error messages (0|1)
	bool		silent,		// true: don't print error messages
	FILE* hexdump	// not NULL: write decrompression hex-dump
)
{
	assert(data);
	assert(dest_buf);
	assert(dest_buf_size);
	//TRACE("DecompressYAZ(vers=%d) src=%p+%zu, dest=%p+%zu\n", yaz_version, data, data_size, dest_buf, dest_buf_size);

	const uint8_t* src = data;
	const uint8_t* src_end = src + data_size;
	uint8_t* dest = dest_buf;
	uint8_t* dest_end = dest + dest_buf_size;
	uint8_t  code = 0;
	int code_len = 0;

	unsigned int count0 = 0;
	unsigned int count1 = 0;
	unsigned int count2 = 0;
	unsigned int count3 = 0;

	int addr_fw = 0;
	char buf[12];
	if (hexdump)
	{
		fprintf(hexdump, "\n"
			"# size of compressed data:    %#8zx = %9zu\n"
			"# size of de-compressed data: %#8zx = %9zu\n"
			"\n",
			data_size, data_size, dest_buf_size, dest_buf_size);
		addr_fw = snprintf(buf, sizeof(buf), "%zx", dest_buf_size - 1) + 1;
	}

	while (src < src_end && dest < dest_end)
	{
		if (!code_len--)
		{
			if (hexdump)
			{
				count0++;
				fprintf(hexdump,
					"%*x: %02x -- -- : type byte\n",
					addr_fw, (unsigned int)(src - (uint8_t*)data), *src);
			}

			code = *src++;
			code_len = 7;
		}

		if (code & 0x80)
		{
			if (hexdump)
			{
				count1++;
				fprintf(hexdump,
					"%*x: %02x -- -- : copy direct\n",
					addr_fw, (unsigned int)(src - (uint8_t*)data), *src);
			}

			// copy 1 byte direct
			*dest++ = *src++;
		}
		else
		{
			// rle part

			const uint8_t b1 = *src++;
			const uint8_t b2 = *src++;
			const uint8_t* copy_src = dest - ((b1 & 0x0f) << 8 | b2) - 1;

			int n = b1 >> 4;
			if (!n)
				n = *src++ + 0x12;
			else
				n += 2;
			assert(n >= 3 && n <= 0x111);

			//noPRINT_IF(copy_src + n > dest, "RLE OVERLAP: copy %zu..%zu -> %zu\n", copy_src - szs->data, copy_src + n - szs->data, dest - szs->data);

			if (copy_src < (uint8_t*)dest_buf)
			{
				if (write_status)
					*write_status = dest - (uint8_t*)dest_buf;
				//if (!silent)
				//	ERROR0(ERR_INVALID_DATA, "YAZ%u data corrupted: Back reference points before beginning of data: %s\n", yaz_version, fname ? fname : "?");
				//return ERR_INVALID_DATA;
			}

			if (dest + n > dest_end)
			{
				// first copy as much as possible
				while (dest < dest_end)
					*dest++ = *copy_src++;

				if (write_status)
					*write_status = dest - (uint8_t*)dest_buf;
				//return silent ? ERR_WARNING : ERROR0(ERR_INVALID_DATA, "YAZ%u data corrupted: Decompressed data larger than specified (%zu>%zu): %s\n", yaz_version, GetDecompressedSizeYAZ(data, data_size), dest_buf_size, fname ? fname : "?");
				return -1;
			}

			if (hexdump)
			{
				if (n < 0x12)
				{
					count2++;
					fprintf(hexdump, "%*x: %02x %02x --", addr_fw, (unsigned int)(src - 2 - (uint8_t*)data), src[-2], src[-1]);
				}
				else
				{
					count3++;
					fprintf(hexdump, "%*x: %02x %02x %02x",
						addr_fw, (unsigned int)(src - 3 - (uint8_t*)data),
						src[-3], src[-2], src[-1]);
				}
				fprintf(hexdump, " : copy %03x off %04d:", n, (unsigned int)(copy_src - dest));
				int max = n < 10 ? n : 10;
				const uint8_t* hex_src = copy_src;

				// copy data before hexdump
				while (n-- > 0)
					*dest++ = *copy_src++;

				while (max-- > 0)
					fprintf(hexdump, " %02x", *hex_src++);
				if (hex_src != copy_src)
					fputs(" ...\n", hexdump);
				else
					fputc('\n', hexdump);
			}
			else
			{
				// don't use memcpy() or memmove() here because
				// they don't work with self referencing chunks.
				while (n-- > 0)
					*dest++ = *copy_src++;
			}
		}

		code <<= 1;
	}
	assert(src <= src_end);
	assert(dest <= dest_end);

	if (hexdump)
		fprintf(hexdump, "\n"
			"# %u type bytes, %u single bytes, %u+%u back references\n"
			"\n",
			count0, count1, count2, count3);

	if (write_status)
		*write_status = dest - (uint8_t*)dest_buf;
	return 0;
}

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

			//decodedAssetCount = 1;
			//decodedAssets[0].assetType = ASSET_TYPE_TEXT;
			//decodedAssets[0].assetPtr = gameFileList[selectedFileIndex].filePtr;
			PAINTSTRUCT ps = { 0 };
			HDC hdc = BeginPaint(hwnd, &ps);
			int nextAssetLocationY = 0;
			for (int i = 0; i < decodedAssetCount; i++)
			{
				switch (decodedAssetTable[i].assetType)
				{
				case ASSET_TYPE_TEXT:
				{
					RECT rect = { 25, si.nPos * -25, 500, 2000 };
					DrawTextA(hdc, decodedAssetTable[i].assetPtr, -1, &rect, DT_LEFT | DT_TOP);
					break;
				}
				case ASSET_TYPE_TEXTURE:
				{
					const struct decodedImage* imgHeader = decodedAssetTable[i].assetPtr;
					BITMAPINFO info = { 0 };
					info.bmiHeader.biBitCount = 32;
					info.bmiHeader.biWidth = imgHeader->width;
					info.bmiHeader.biHeight = imgHeader->height;
					info.bmiHeader.biPlanes = 1;
					info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
					info.bmiHeader.biSizeImage = imgHeader->pixelCount * 4;

					// Draw the pixel
					StretchDIBits(hdc, 0, nextAssetLocationY, imgHeader->width * 4, imgHeader->height * 4, 0, 0, imgHeader->width, imgHeader->height, imgHeader->pixels, &info, DIB_RGB_COLORS, SRCCOPY);
					nextAssetLocationY += imgHeader->height * 4;
					break;
				}
				}
			}
			

			EndPaint(hwnd, &ps);
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

						int decompressionStatus = DecompressYAZ(
							src,//data
							gameFileList[selectedFileIndex].fileSize - sizeof(struct yaz0Header),//data size
							dest,//dest buf
							SwapEndian(header->uncompressedSize),
							nullptr,//write status
							0,//yaz version (pikmin 2 uses yaz0)
							true,
							nullptr
							);

						printf("decompression status: %i\n", decompressionStatus);

						//for (int j = 180; j < 512; j++)
						//for (int j = 192; j < 512; j++)
						//for (int j = 224; j < 512; j++)
						//{
						//	printf("%c", dest[j]);
						//}

						//printf("\n");

						//todo: sort different files by type, but for now just assume its always bmd

						{
							struct J3DFileHeader {
								uint32_t J3DVersion;
								uint32_t fileVersion;
								uint8_t unknown1[4];
								uint32_t blockCount;
								uint8_t unknown2[16];
							};

							//todo: temporary hardcoded offset
							const struct J3DFileHeader* bmdFileHeader = OffsetPointer(dest, 192);

							printf("block count: %i\n", SwapEndian(bmdFileHeader->blockCount));

							const void* bmdFile = OffsetPointer(bmdFileHeader, sizeof(struct J3DFileHeader));
							decodedAssetCount = 0;

							void* decodedAssetFreeZone = decodedAssetData;


							//iterate over bmd sections
							struct bmdSection
							{
								char chunkType[4];
								int32_t size;
							};
							const struct bmdSection* currentSection = bmdFile;

							for (int i = 0; i < SwapEndian(bmdFileHeader->blockCount); i++)
							{
								printf("chunk type: %c%c%c%c\n",
									currentSection->chunkType[0],
									currentSection->chunkType[1],
									currentSection->chunkType[2],
									currentSection->chunkType[3]
								);

								if (
									currentSection->chunkType[0] == 'I' &&
									currentSection->chunkType[1] == 'N' &&
									currentSection->chunkType[2] == 'F' &&
									currentSection->chunkType[3] == '1'
									)
								{
									struct INF1
									{
										char chunkType[4];
										int32_t size;
										int16_t miscFlags;
										int16_t padding;
										int32_t matrixGroupCount;
										int32_t vertexCount;
										int32_t hierarchyDataOffset;
									};

									const struct INF1* header = currentSection;

									printf("size: %i\n", SwapEndian(header->size));

									printf("vertex count: %i\n", SwapEndian(header->vertexCount));

									printf("hierarchy data offset: %i\n", SwapEndian(header->hierarchyDataOffset));

									struct hierarchyNode
									{
										short NodeType;
										short Data;
									};

									const struct hierarchyNode* bmdHierarchy = OffsetPointer(bmdFile, SwapEndian(header->hierarchyDataOffset));

									int hierarchyNodeDepth = 0;
									for (int hierarchyNodeIndex = 0; bmdHierarchy[hierarchyNodeIndex].NodeType != 0x00; hierarchyNodeIndex++)
									{
										for (int i = 0; i < hierarchyNodeDepth; i++)
											printf("\t");

										switch (SwapEndian(bmdHierarchy[hierarchyNodeIndex].NodeType))
										{
										case 0x00:
											break;
										case 0x01:
											printf("new node\n");;
											hierarchyNodeDepth++;
											break;
										case 0x02:
											printf("end of node\n");
											hierarchyNodeDepth--;
											break;
										case 0x10:
											printf("joint (0x%X)\n", SwapEndian(SwapEndian(bmdHierarchy[hierarchyNodeIndex].Data)));
											break;
										case 0x11:
											printf("material (0x%X)\n", SwapEndian(SwapEndian(bmdHierarchy[hierarchyNodeIndex].Data)));
											break;
										case 0x12:
											printf("shape (0x%X)\n", SwapEndian(SwapEndian(bmdHierarchy[hierarchyNodeIndex].Data)));
											break;
										default:
											DebugBreak();
											break;
										}
									}

									printf("end of hierarchy\n");
								}
								else if (
									currentSection->chunkType[0] == 'V' &&
									currentSection->chunkType[1] == 'T' &&
									currentSection->chunkType[2] == 'X' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading VTX1\n");
								}
								else if (
									currentSection->chunkType[0] == 'E' &&
									currentSection->chunkType[1] == 'V' &&
									currentSection->chunkType[2] == 'P' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading EVP1\n");
								}
								else if (
									currentSection->chunkType[0] == 'D' &&
									currentSection->chunkType[1] == 'R' &&
									currentSection->chunkType[2] == 'W' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading DRW1\n");
								}
								else if (
									currentSection->chunkType[0] == 'J' &&
									currentSection->chunkType[1] == 'N' &&
									currentSection->chunkType[2] == 'T' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading JNT1\n");
								}
								else if (
									currentSection->chunkType[0] == 'S' &&
									currentSection->chunkType[1] == 'H' &&
									currentSection->chunkType[2] == 'P' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading SHP1\n");
								}
								else if (
									currentSection->chunkType[0] == 'M' &&
									currentSection->chunkType[1] == 'A' &&
									currentSection->chunkType[2] == 'T' &&
									currentSection->chunkType[3] == '3')
								{
									printf("reading MAT3\n");
								}
								else if (
									currentSection->chunkType[0] == 'T' &&
									currentSection->chunkType[1] == 'E' &&
									currentSection->chunkType[2] == 'X' &&
									currentSection->chunkType[3] == '1')
								{
									printf("reading TEX1\n");

									struct TEX1
									{
										char chunkType[4];
										int32_t size;
										uint16_t textureCount;
										uint16_t padding;
										int32_t textureHeaderOffset;
										int32_t stringTableOffset;
									};


									const struct TEX1* header = currentSection;


									printf("texture count: %i\n", SwapEndian(header->textureCount));

									struct BTI
									{
										int8_t format;
										bool alphaEnabled;
										int16_t width;
										int16_t height;
										int8_t wrapS;
										int8_t wrapT;
										bool palettesEnabled;
										int8_t palletteFormat;
										int16_t palletteCount;
										int32_t palletteOffset;
										bool mipsEnabled;
										bool doEdgeLOD;
										bool biasClamp;
										int8_t maxAnisotropy;
										int8_t GXMinFilter;
										int8_t GXMaxFilter;
										int8_t MinLOD;
										int8_t MaxLOD;
										int8_t mipCount;
										int8_t unknown;
										int16_t LODBias;
										int32_t textureDataOffset;
									};

									const struct BTI* BTIHeaderTable = OffsetPointer(header, SwapEndian(header->textureHeaderOffset));

									for (int texNum = 0; texNum < SwapEndian(header->textureCount); texNum++)
									{
										printf("texture format: 0x%X\n", + BTIHeaderTable[texNum].format);
										printf("texture size: %i x %i\n", SwapEndian(BTIHeaderTable[texNum].width), SwapEndian(BTIHeaderTable[texNum].height));
										printf("offset in file: 0x%X\n", SwapEndian(BTIHeaderTable[texNum].textureDataOffset));

										struct decodedImage* imageHeader = decodedAssetFreeZone;
										decodedAssetFreeZone = OffsetPointer(decodedAssetFreeZone, sizeof(struct decodedImage));

										imageHeader->width = SwapEndian(BTIHeaderTable[texNum].width);
										imageHeader->height = SwapEndian(BTIHeaderTable[texNum].height);
										imageHeader->pixelCount = SwapEndian(BTIHeaderTable[texNum].width) * SwapEndian(BTIHeaderTable[texNum].height);

										imageHeader->pixels = decodedAssetFreeZone;
										decodedAssetFreeZone += imageHeader->pixelCount * 4;
										decodeTexture(imageHeader->width, imageHeader->height, imageHeader->pixelCount, OffsetPointer(&BTIHeaderTable[texNum], SwapEndian(BTIHeaderTable[texNum].textureDataOffset)), imageHeader->pixels, BTIHeaderTable->format);


										decodedAssetTable[decodedAssetCount].assetType = ASSET_TYPE_TEXTURE;
										decodedAssetTable[decodedAssetCount].assetPtr = imageHeader;
										decodedAssetCount++;
									}
								}
								else
								{
									DebugBreak();
								}


								currentSection = OffsetPointer(currentSection, SwapEndian(currentSection->size));
							}
						}
					}
					else if (wcscmp(extensionType, L".txt") == 0)
					{
						displayedFileType = 1;

						//new ver
						decodedAssetCount = 1;
						decodedAssetTable[0].assetType = ASSET_TYPE_TEXT;
						decodedAssetTable[0].assetPtr = gameFileList[selectedFileIndex].filePtr;

					}
					else if (wcscmp(extensionType, L".ini") == 0)
					{
						displayedFileType = 2;

						//new ver
						decodedAssetCount = 1;
						decodedAssetTable[0].assetType = ASSET_TYPE_TEXT;
						decodedAssetTable[0].assetPtr = gameFileList[selectedFileIndex].filePtr;
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
	decodedAssetTable = malloc(sizeof(struct decodedAsset) * 32);//maximum 32 assets per file?
	//todo: use dynamically allocated array instead
	decodedAssetData = malloc(1024 * 1024 * 24);
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
