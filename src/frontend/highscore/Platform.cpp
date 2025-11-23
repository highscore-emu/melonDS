/*
    Copyright 2016-2022 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "melonds-highscore.h"

#include "Platform.h"

#include <mutex>
#include <thread>

#include <semaphore.h>

#include "../mic_blow.h"

#define MIC_SAMPLE_LENGTH 735

namespace melonDS::Platform
{

void SignalStop(StopReason reason, void* userdata)
{
}

int InstanceID()
{
    return 0;
}

constexpr char AccessMode(FileMode mode, bool file_exists)
{
    if (mode & FileMode::Append)
        return  'a';

    if (!(mode & FileMode::Write))
        // If we're only opening the file for reading...
        return 'r';

    if (mode & (FileMode::NoCreate))
        // If we're not allowed to create a new file...
        return 'r'; // Open in "r+" mode (IsExtended will add the "+")

    if ((mode & FileMode::Preserve) && file_exists)
        // If we're not allowed to overwrite a file that already exists...
        return 'r'; // Open in "r+" mode (IsExtended will add the "+")

    return 'w';
}

constexpr bool IsExtended(FileMode mode)
{
    // fopen's "+" flag always opens the file for read/write
    return (mode & FileMode::ReadWrite) == FileMode::ReadWrite;
}

static std::string GetModeString(FileMode mode, bool file_exists)
{
    std::string modeString;

    modeString += AccessMode(mode, file_exists);

    if (IsExtended(mode))
        modeString += '+';

    if (!(mode & FileMode::Text))
        modeString += 'b';

    return modeString;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    g_autoptr (GFile) file = g_file_new_for_path (path.c_str ());
    gboolean file_exists = g_file_query_exists (file, NULL);
    std::string mode_string = GetModeString (mode, file_exists);

    return (FileHandle*) fopen (path.c_str (), mode_string.c_str ());
}

std::string GetLocalFilePath(const std::string& filename)
{
    if (filename == "shadercache") {
        const char *cache_path = melonds_core_get_cache_path ();

        return std::string (cache_path) + "/" + filename;
    }

    return filename;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile (GetLocalFilePath (path), mode);
}

bool CloseFile(FileHandle* file)
{
    return fclose(reinterpret_cast<FILE *>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(reinterpret_cast<FILE *>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, reinterpret_cast<FILE *>(file)) != nullptr;
}

bool FileExists(const std::string& name)
{
    FileHandle* f = OpenFile (name, FileMode::Read);
    if (!f) return false;
    CloseFile (f);
    return true;
}

bool LocalFileExists(const std::string& name)
{
    FileHandle* f = OpenLocalFile (name, FileMode::Read);
    if (!f) return false;
    CloseFile (f);
    return true;
}

bool CheckFileWritable(const std::string& filepath)
{
    FileHandle* file = Platform::OpenFile(filepath.c_str(), FileMode::Read);

    if (file)
    {
        // if the file exists, check if it can be opened for writing.
        Platform::CloseFile(file);
        file = Platform::OpenFile(filepath.c_str(), FileMode::Append);
        if (file)
        {
            Platform::CloseFile(file);
            return true;
        }
        else return false;
    }
    else
    {
        return true;
    }
}

bool CheckLocalFileWritable(const std::string& name)
{
    FileHandle* file = Platform::OpenLocalFile (name.c_str (), FileMode::Append);
    if (file)
    {
        Platform::CloseFile (file);
        return true;
    }
    else return false;
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int stdorigin;
    switch (origin)
    {
        case FileSeekOrigin::Start: stdorigin = SEEK_SET; break;
        case FileSeekOrigin::Current: stdorigin = SEEK_CUR; break;
        case FileSeekOrigin::End: stdorigin = SEEK_END; break;
    }

    return fseek(reinterpret_cast<FILE *>(file), offset, stdorigin) == 0;
}

void FileRewind(FileHandle* file)
{
    rewind(reinterpret_cast<FILE *>(file));
}

u64 FilePosition(FileHandle* file)
{
    return ftell(reinterpret_cast<FILE *>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, reinterpret_cast<FILE *>(file));
}

bool FileFlush(FileHandle* file)
{
    return fflush(reinterpret_cast<FILE *>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, reinterpret_cast<FILE *>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (fmt == nullptr)
        return 0;

    va_list args;
    va_start(args, fmt);
    u64 ret = vfprintf(reinterpret_cast<FILE *>(file), fmt, args);
    va_end(args);
    return ret;
}

u64 FileLength(FileHandle* file)
{
    FILE* stdfile = reinterpret_cast<FILE *>(file);
    long pos = ftell(stdfile);
    fseek(stdfile, 0, SEEK_END);
    long len = ftell(stdfile);
    fseek(stdfile, pos, SEEK_SET);
    return len;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (fmt == nullptr)
        return;

    va_list args;
    va_start(args, fmt);
    g_autofree char *message = g_strdup_vprintf (fmt, args);
    va_end(args);

    HsLogLevel hs_level;
    switch (level) {
    case Debug:
      hs_level = HS_LOG_DEBUG;
      break;
    case Info:
      hs_level = HS_LOG_INFO;
      break;
    case Warn:
      hs_level = HS_LOG_WARNING;
      break;
    case Error:
      hs_level = HS_LOG_CRITICAL;
      break;
    }

    int len = strlen (message);
    if (message[len - 1] == '\n')
      message[len - 1] = '\0';

    melonds_core_log (hs_level, message);
}

Thread* Thread_Create(std::function<void()> func)
{
    return (Thread*) new std::thread (func);
}

void Thread_Free(Thread* thread)
{
    if (((std::thread*) thread)->joinable ())
        ((std::thread*) thread)->join ();

    delete (std::thread*) thread;
}

void Thread_Wait(Thread* thread)
{
    ((std::thread*) thread)->join ();
}

Semaphore* Semaphore_Create()
{
    sem_t *sem = g_new0 (sem_t, 1);
    sem_init (sem, 0, 1);

    return (Semaphore*) sem;
}

void Semaphore_Free(Semaphore* sema)
{
    sem_destroy ((sem_t*) sema);
    g_free (sema);
}

void Semaphore_Reset(Semaphore* sema)
{
    int value;
    sem_getvalue ((sem_t*) sema, &value);

    for (int i = 0; i < value; i++)
        Semaphore_Wait (sema);
}

void Semaphore_Wait(Semaphore* sema)
{
    sem_wait ((sem_t*) sema);
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    if (!timeout_ms)
        return sem_trywait ((sem_t*) sema);

    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = timeout_ms * 1000000;

    return sem_timedwait ((sem_t*) sema, &ts);
}

void Semaphore_Post(Semaphore* sema, int count)
{
    for (int i = 0; i < count; i++)
        sem_post ((sem_t*) sema);
}

Mutex* Mutex_Create()
{
    return (Mutex*) new std::mutex;
}

void Mutex_Free(Mutex* mutex)
{
    delete (std::mutex*) mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    ((std::mutex*) mutex)->lock ();
}

void Mutex_Unlock(Mutex* mutex)
{
    ((std::mutex*) mutex)->unlock ();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return ((std::mutex*) mutex)->try_lock ();
}

void Sleep(u64 usecs)
{
    g_usleep (usecs);
}

u64 GetMSCount()
{
    return 0; // This is only needed for netplay
}

u64 GetUSCount()
{
    return 0; // This is only needed for netplay
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    const char *save_path = melonds_core_get_save_path ();
    g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);
    g_autoptr (GFile) save_file = g_file_get_child (save_dir, "save.sav");
    g_autoptr (GError) error = NULL;

    if (!g_file_replace_contents (save_file, (char*) savedata, savelen, NULL, FALSE,
                                  G_FILE_CREATE_NONE, NULL, NULL, &error)) {
      g_autofree char *message = g_strdup_printf ("Failed to save NDS data: %s", error->message);
      melonds_core_log (HS_LOG_CRITICAL, message);
      return;
    }
}

void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata)
{
    // We'll need this if we boot into BIOS and can use the built-in date/time picker
}

void MP_Begin(void* userdata)
{
}

void MP_End(void* userdata)
{
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    return 0;
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    return 0;
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    return 0;
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    return 0;
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    return 0;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    return 0;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    return 0;
}

int Net_SendPacket(u8* data, int len, void* userdata)
{
    return 0;
}

int Net_RecvPacket(u8* data, void* userdata)
{
    return 0;
}

void Mic_Start(void* userdata)
{
}

void Mic_Stop(void* userdata)
{
}

int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
  static int sample_pos = 0;

  if (!melonds_core_get_mic_active ()) {
    sample_pos = 0;
    memset (data, 0, maxlength * sizeof (s16));
    return maxlength;
  }

  int read_length = 0;
  int sample_len = sizeof (mic_blow) / sizeof (s16);

  while (read_length < maxlength) {
    int this_len = maxlength - read_length;
    if ((sample_pos + this_len) > sample_len)
      this_len = sample_len - sample_pos;

    if (!this_len)
      break;

    memcpy (data, mic_blow, this_len * sizeof (s16));
    data += this_len;
    sample_pos += this_len;
    if (sample_pos >= sample_len)
      sample_pos -= sample_len;

    read_length += this_len;
  }

  return read_length;
}

void Camera_Start(int num, void* userdata)
{
}

void Camera_Stop(int num, void* userdata)
{
}

void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata)
{
}

bool Addon_KeyDown(KeyType type, void* userdata)
{
    return false;
}


void Addon_RumbleStart(u32 len, void* userdata)
{
  HsCore *core = HS_CORE (userdata);

  hs_core_rumble (core, 0, 1, 1, (u16) len);
}

void Addon_RumbleStop(void* userdata)
{
  HsCore *core = HS_CORE (userdata);

  hs_core_rumble (core, 0, 0, 0, 0);
}

float Addon_MotionQuery(MotionQueryType type, void* userdata)
{
    return 0.0;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib)
{
    return nullptr;
}

void DynamicLibrary_Unload(DynamicLibrary* lib)
{
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
    return nullptr;
}

}
