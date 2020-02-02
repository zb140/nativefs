#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN

#include <stdio.h>
#include <io.h>
#include <windows.h> // for FlushFileBuffers

/* this is a totally hokey "implementation" of fsync, but
 * it works well enough.  Specifically, it doesn't bother
 * reporting errors since the below code doesn't check
 * for them anyway.
 */
void fsync(int fd) {
    HANDLE h = (HANDLE) _get_osfhandle(fd);

    if (h == INVALID_HANDLE_VALUE) return;

    FlushFileBuffers(h);
}

#else
#include <unistd.h>
#endif

#include <fcntl.h>
#include <string>
#include <string.h>

#include <node.h>
#include <nan.h>

#ifdef _WIN32
#define open  _open
#define read  _read
#define write _write
#define close _close
#define lseek _lseeki64

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#define O_BINARY _O_BINARY

// needed to support files > 4GB
#define stat __stat64
#define fstat _fstat64
#else
#define O_BINARY 0
#endif

namespace NativeFS {
    using LocalValue = v8::Local<v8::Value>;

    std::string get(LocalValue value) {
        if (!value->IsString()) {
            Nan::ThrowTypeError("arg must be a string");
            return "";
        }
        Nan::Utf8String utf8_value(value);
        int len = utf8_value.length();
        if (len <= 0) {
            Nan::ThrowTypeError("arg must be a non-empty string");
            return "";
        }
        return std::string(*utf8_value, len);
    }

    template <typename T>
        class Property {
            public:
                virtual ~Property() {}
                virtual operator T const & () const { return value; }

                virtual T const * operator-> () const { return &value; }

            protected:
                T value;

                friend class Args;
                virtual T& operator=(const T& f) { return value = f; }
        };

    class StringProperty : public Property<std::string> {
        public:
            using Property<std::string>::operator->;

            virtual operator const char* () const { return value.c_str(); }

        protected:
            friend class Args;
            using Property<std::string>::operator=;
    };

    class Args {
        public:
            StringProperty Source;
            StringProperty Destination;

            Property<v8::Local<v8::Function>> ProgressCallback;
            Property<v8::Local<v8::Function>> ResultCallback;

            Property<bool> UpdateProgress;

            Args(const Nan::FunctionCallbackInfo<v8::Value>& args) {
                if (args.Length() < 3) {
                    Nan::ThrowError("Not enough arguments");
                    return;
                }
                if (!args[0]->IsString()) {
                    Nan::ThrowTypeError("First argument is not a path");
                    return;
                }
                if (!args[1]->IsString()) {
                    Nan::ThrowTypeError("Second argument is not a path");
                    return;
                }
                if (!args[2]->IsFunction()) {
                    Nan::ThrowError("Missing result callback");
                    return;
                }
                if (args.Length() > 3 && !args[3]->IsFunction()) {
                    Nan::ThrowError("Unknown arguments");
                    return;
                }

                UpdateProgress = args.Length() > 3;

                Source      = get(args[0]);
                Destination = get(args[1]);

                ResultCallback   = (UpdateProgress ? args[3] : args[2]).As<v8::Function>();
                ProgressCallback = args[2].As<v8::Function>();
            }
    };

    void SendProgressUpdate(const Args& args, double completed, double total) {
        if (args.UpdateProgress) {
            LocalValue argv[2] = {
                Nan::New<v8::Number>(completed),
                Nan::New<v8::Number>(total),
            };
            Nan::Call(
                Nan::Callback{ args.ProgressCallback },
                2, argv
            );
        }
    }

    void SendComplete(const Args& args) {
        LocalValue argv[2] = { Nan::Null(), Nan::True() };
        Nan::Call(
            Nan::Callback{ args.ResultCallback },
            2, argv
        );
    }

    void SendError(const Args& args, const char* errorText) {
        LocalValue argv[2] = {
            Nan::New<v8::String>(errorText).ToLocalChecked(),
            Nan::False()
        };
        Nan::Call(
            Nan::Callback{ args.ResultCallback },
            2, argv
        );
    }

    int doWrite(int fd, char *data, int datalen) {
        int written = write(fd, data, datalen);
        if (written == -1) return -1;
        if (written < datalen) {
            return doWrite(fd, data + written, datalen - written);
        }
        return written;
    }

    const int BUFFER_SIZE = 16384;
    void Copy(
        int fd_in, int fd_out, ssize_t inputSize,
        const Args& args, bool removeWhenDone = false
    ) {
        char buffer[BUFFER_SIZE];

        const ssize_t bytesPerUpdate = inputSize / 100;

        ssize_t progress = 0;
        ssize_t bytes_read = 0;
        ssize_t sinceLastUpdate = 0;

        while ((bytes_read = read(fd_in, buffer, sizeof(buffer))) > 0)
        {
            ssize_t written = doWrite(fd_out, buffer, bytes_read);
            if (written == -1) goto copyByFdError;

            progress += bytes_read;
            sinceLastUpdate += bytes_read;

            if (sinceLastUpdate > bytesPerUpdate) {
                SendProgressUpdate(args, (double) progress, (double) inputSize);
                sinceLastUpdate = 0;
            }
        }

        if (bytes_read == -1) goto copyByFdError;

        // send one last progress update
        SendProgressUpdate(args, (double) inputSize, (double) inputSize);

        close(fd_in);

        fsync(fd_out); // Flush
        close(fd_out);

        if (removeWhenDone) {
            remove(args.Source);
        }

        SendComplete(args);
        return;

    copyByFdError:
        close(fd_in);
        close(fd_out);

        remove(args.Destination); // remove failed copy
        SendError(args, strerror(errno));
        return;
    }

    NAN_METHOD(Copy) {
        Args args(info);

        int out, in = open(args.Source, O_RDONLY | O_BINARY);
        if (in < 0) goto copyByPathError;


        struct stat st;

        // Get the input file information
        if (fstat(in, &st) != 0) {
            close(in);
            goto copyByPathError;
        }

        out = open(args.Destination, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, st.st_mode);
        if (out < 0) {
            close(in);
            goto copyByPathError;
        }

        Copy(in, out, st.st_size, args);
        return;

    copyByPathError:
        remove(args.Destination); // remove failed copy
        SendError(args, strerror(errno));
        return;
    }

    NAN_METHOD(Move) {
        Args args(info);

        int out, in = open(args.Source, O_RDONLY | O_BINARY);
        if (in < 0) goto moveError;

        struct stat in_stats;

        // Get the input file information
        if (fstat(in, &in_stats) != 0) {
            close(in);
            goto moveError;
        }

        // Open target
        out = open(args.Destination, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, in_stats.st_mode);
        if (out < 0) {
            close(in);
            goto moveError;
        }

        struct stat out_stats;

        // Get the output file information
        if (fstat(out, &out_stats) != 0) {
            close(in);
            close(out);
            goto moveError;
        }

        {
            const ssize_t inputSize = in_stats.st_size;
            if (in_stats.st_dev == out_stats.st_dev) {
                close(in);
                close(out);

                // These files are on the same device; it would
                // be much quicker to just rename the file
                remove(args.Destination);
                rename(args.Source, args.Destination);

                SendProgressUpdate(args, (double) inputSize, (double) inputSize);
                SendComplete(args);
            }
            else {
                // They're on different devices.  We'll need to
                // do this as a copy followed by a remove.
                Copy(in, out, inputSize, args, /* removeWhenDone: */ true);
            }
        }
        return;

    moveError:
        remove(args.Destination); // remove failed copy
        SendError(args, strerror(errno));
        return;
    }

    NAN_MODULE_INIT(InitAll) {
        Nan::Set(target,
            Nan::New<v8::String>("copy").ToLocalChecked(),
            Nan::GetFunction(Nan::New<v8::FunctionTemplate>(Copy)).ToLocalChecked()
        );
        Nan::Set(target,
            Nan::New<v8::String>("move").ToLocalChecked(),
            Nan::GetFunction(Nan::New<v8::FunctionTemplate>(Move)).ToLocalChecked()
        );
    }

    NODE_MODULE(native_fs, InitAll)
}  // namespace NativeFS

#ifdef _WIN32
#undef open
#undef read
#undef write
#undef close

#undef O_RDONLY
#undef O_WRONLY
#undef O_CREAT
#undef O_TRUNC

#undef stat
#undef fstat
#endif

#undef O_BINARY

