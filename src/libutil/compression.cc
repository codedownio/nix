#include "compression.hh"
#include "util.hh"
#include "finally.hh"
#include "logging.hh"

#include <lzma.h>
#include <bzlib.h>
#include <cstdio>
#include <cstring>

#include <brotli/decode.h>
#include <brotli/encode.h>

#include <zlib.h>

#include <iostream>

namespace nix {

// Don't feed brotli too much at once.
struct ChunkedCompressionSink : CompressionSink
{
    uint8_t outbuf[32 * 1024];

    void write(std::string_view data) override
    {
        const size_t CHUNK_SIZE = sizeof(outbuf) << 2;
        while (!data.empty()) {
            size_t n = std::min(CHUNK_SIZE, data.size());
            writeInternal(data);
            data.remove_prefix(n);
        }
    }

    virtual void writeInternal(std::string_view data) = 0;
};

struct NoneSink : CompressionSink
{
    Sink & nextSink;
    NoneSink(Sink & nextSink) : nextSink(nextSink) { }
    void finish() override { flush(); }
    void write(std::string_view data) override { nextSink(data); }
};

struct GzipDecompressionSink : CompressionSink
{
    Sink & nextSink;
    z_stream strm;
    bool finished = false;
    uint8_t outbuf[BUFSIZ];

    GzipDecompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        strm.next_out = outbuf;
        strm.avail_out = sizeof(outbuf);

        // Enable gzip and zlib decoding (+32) with 15 windowBits
        int ret = inflateInit2(&strm,15+32);
        if (ret != Z_OK)
            throw CompressionError("unable to initialise gzip encoder");
    }

    ~GzipDecompressionSink()
    {
        inflateEnd(&strm);
    }

    void finish() override
    {
        CompressionSink::flush();
        write({});
    }

    void write(std::string_view data) override
    {
        assert(data.size() <= std::numeric_limits<decltype(strm.avail_in)>::max());

        strm.next_in = (Bytef *) data.data();
        strm.avail_in = data.size();

        while (!finished && (!data.data() || strm.avail_in)) {
            checkInterrupt();

            int ret = inflate(&strm,Z_SYNC_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END)
                throw CompressionError("error while decompressing gzip file: %d (%d, %d)",
                    zError(ret), data.size(), strm.avail_in);

            finished = ret == Z_STREAM_END;

            if (strm.avail_out < sizeof(outbuf) || strm.avail_in == 0) {
                nextSink({(char *) outbuf, sizeof(outbuf) - strm.avail_out});
                strm.next_out = (Bytef *) outbuf;
                strm.avail_out = sizeof(outbuf);
            }
        }
    }
};

struct XzDecompressionSink : CompressionSink
{
    Sink & nextSink;
    uint8_t outbuf[BUFSIZ];
    lzma_stream strm = LZMA_STREAM_INIT;
    bool finished = false;

    XzDecompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        lzma_ret ret = lzma_stream_decoder(
            &strm, UINT64_MAX, LZMA_CONCATENATED);
        if (ret != LZMA_OK)
            throw CompressionError("unable to initialise lzma decoder");

        strm.next_out = outbuf;
        strm.avail_out = sizeof(outbuf);
    }

    ~XzDecompressionSink()
    {
        lzma_end(&strm);
    }

    void finish() override
    {
        CompressionSink::flush();
        write({});
    }

    void write(std::string_view data) override
    {
        strm.next_in = (const unsigned char *) data.data();
        strm.avail_in = data.size();

        while (!finished && (!data.data() || strm.avail_in)) {
            checkInterrupt();

            lzma_ret ret = lzma_code(&strm, data.data() ? LZMA_RUN : LZMA_FINISH);
            if (ret != LZMA_OK && ret != LZMA_STREAM_END)
                throw CompressionError("error %d while decompressing xz file", ret);

            finished = ret == LZMA_STREAM_END;

            if (strm.avail_out < sizeof(outbuf) || strm.avail_in == 0) {
                nextSink({(char *) outbuf, sizeof(outbuf) - strm.avail_out});
                strm.next_out = outbuf;
                strm.avail_out = sizeof(outbuf);
            }
        }
    }
};

struct BzipDecompressionSink : ChunkedCompressionSink
{
    Sink & nextSink;
    bz_stream strm;
    bool finished = false;

    BzipDecompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        memset(&strm, 0, sizeof(strm));
        int ret = BZ2_bzDecompressInit(&strm, 0, 0);
        if (ret != BZ_OK)
            throw CompressionError("unable to initialise bzip2 decoder");

        strm.next_out = (char *) outbuf;
        strm.avail_out = sizeof(outbuf);
    }

    ~BzipDecompressionSink()
    {
        BZ2_bzDecompressEnd(&strm);
    }

    void finish() override
    {
        flush();
        write({});
    }

    void writeInternal(std::string_view data) override
    {
        assert(data.size() <= std::numeric_limits<decltype(strm.avail_in)>::max());

        strm.next_in = (char *) data.data();
        strm.avail_in = data.size();

        while (strm.avail_in) {
            checkInterrupt();

            int ret = BZ2_bzDecompress(&strm);
            if (ret != BZ_OK && ret != BZ_STREAM_END)
                throw CompressionError("error while decompressing bzip2 file");

            finished = ret == BZ_STREAM_END;

            if (strm.avail_out < sizeof(outbuf) || strm.avail_in == 0) {
                nextSink({(char *) outbuf, sizeof(outbuf) - strm.avail_out});
                strm.next_out = (char *) outbuf;
                strm.avail_out = sizeof(outbuf);
            }
        }
    }
};

struct BrotliDecompressionSink : ChunkedCompressionSink
{
    Sink & nextSink;
    BrotliDecoderState * state;
    bool finished = false;

    BrotliDecompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!state)
            throw CompressionError("unable to initialize brotli decoder");
    }

    ~BrotliDecompressionSink()
    {
        BrotliDecoderDestroyInstance(state);
    }

    void finish() override
    {
        flush();
        writeInternal({});
    }

    void writeInternal(std::string_view data) override
    {
        auto next_in = (const uint8_t *) data.data();
        size_t avail_in = data.size();
        uint8_t * next_out = outbuf;
        size_t avail_out = sizeof(outbuf);

        while (!finished && (!data.data() || avail_in)) {
            checkInterrupt();

            if (!BrotliDecoderDecompressStream(state,
                    &avail_in, &next_in,
                    &avail_out, &next_out,
                    nullptr))
                throw CompressionError("error while decompressing brotli file");

            if (avail_out < sizeof(outbuf) || avail_in == 0) {
                nextSink({(char *) outbuf, sizeof(outbuf) - avail_out});
                next_out = outbuf;
                avail_out = sizeof(outbuf);
            }

            finished = BrotliDecoderIsFinished(state);
        }
    }
};

ref<std::string> decompress(const std::string & method, const std::string & in)
{
    StringSink ssink;
    auto sink = makeDecompressionSink(method, ssink);
    (*sink)(in);
    sink->finish();
    return ssink.s;
}

ref<CompressionSink> makeDecompressionSink(const std::string & method, Sink & nextSink)
{
    if (method == "none" || method == "")
        return make_ref<NoneSink>(nextSink);
    else if (method == "xz")
        return make_ref<XzDecompressionSink>(nextSink);
    else if (method == "bzip2")
        return make_ref<BzipDecompressionSink>(nextSink);
    else if (method == "gzip")
        return make_ref<GzipDecompressionSink>(nextSink);
    else if (method == "br")
        return make_ref<BrotliDecompressionSink>(nextSink);
    else
        throw UnknownCompressionMethod("unknown compression method '%s'", method);
}

struct XzCompressionSink : CompressionSink
{
    Sink & nextSink;
    uint8_t outbuf[BUFSIZ];
    lzma_stream strm = LZMA_STREAM_INIT;
    bool finished = false;

    XzCompressionSink(Sink & nextSink, bool parallel) : nextSink(nextSink)
    {
        lzma_ret ret;
        bool done = false;

        if (parallel) {
#ifdef HAVE_LZMA_MT
            lzma_mt mt_options = {};
            mt_options.flags = 0;
            mt_options.timeout = 300; // Using the same setting as the xz cmd line
            mt_options.preset = LZMA_PRESET_DEFAULT;
            mt_options.filters = NULL;
            mt_options.check = LZMA_CHECK_CRC64;
            mt_options.threads = lzma_cputhreads();
            mt_options.block_size = 0;
            if (mt_options.threads == 0)
                mt_options.threads = 1;
            // FIXME: maybe use lzma_stream_encoder_mt_memusage() to control the
            // number of threads.
            ret = lzma_stream_encoder_mt(&strm, &mt_options);
            done = true;
#else
            printMsg(lvlError, "warning: parallel XZ compression requested but not supported, falling back to single-threaded compression");
#endif
        }

        if (!done)
            ret = lzma_easy_encoder(&strm, 6, LZMA_CHECK_CRC64);

        if (ret != LZMA_OK)
            throw CompressionError("unable to initialise lzma encoder");

        // FIXME: apply the x86 BCJ filter?

        strm.next_out = outbuf;
        strm.avail_out = sizeof(outbuf);
    }

    ~XzCompressionSink()
    {
        lzma_end(&strm);
    }

    void finish() override
    {
        CompressionSink::flush();
        write({});
    }

    void write(std::string_view data) override
    {
        strm.next_in = (const unsigned char *) data.data();
        strm.avail_in = data.size();

        while (!finished && (!data.data() || strm.avail_in)) {
            checkInterrupt();

            lzma_ret ret = lzma_code(&strm, data.data() ? LZMA_RUN : LZMA_FINISH);
            if (ret != LZMA_OK && ret != LZMA_STREAM_END)
                throw CompressionError("error %d while compressing xz file", ret);

            finished = ret == LZMA_STREAM_END;

            if (strm.avail_out < sizeof(outbuf) || strm.avail_in == 0) {
                nextSink({(const char *) outbuf, sizeof(outbuf) - strm.avail_out});
                strm.next_out = outbuf;
                strm.avail_out = sizeof(outbuf);
            }
        }
    }
};

struct BzipCompressionSink : ChunkedCompressionSink
{
    Sink & nextSink;
    bz_stream strm;
    bool finished = false;

    BzipCompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        memset(&strm, 0, sizeof(strm));
        int ret = BZ2_bzCompressInit(&strm, 9, 0, 30);
        if (ret != BZ_OK)
            throw CompressionError("unable to initialise bzip2 encoder");

        strm.next_out = (char *) outbuf;
        strm.avail_out = sizeof(outbuf);
    }

    ~BzipCompressionSink()
    {
        BZ2_bzCompressEnd(&strm);
    }

    void finish() override
    {
        flush();
        writeInternal({});
    }

    void writeInternal(std::string_view data) override
    {
        assert(data.size() <= std::numeric_limits<decltype(strm.avail_in)>::max());

        strm.next_in = (char *) data.data();
        strm.avail_in = data.size();

        while (!finished && (!data.data() || strm.avail_in)) {
            checkInterrupt();

            int ret = BZ2_bzCompress(&strm, data.data() ? BZ_RUN : BZ_FINISH);
            if (ret != BZ_RUN_OK && ret != BZ_FINISH_OK && ret != BZ_STREAM_END)
                throw CompressionError("error %d while compressing bzip2 file", ret);

            finished = ret == BZ_STREAM_END;

            if (strm.avail_out < sizeof(outbuf) || strm.avail_in == 0) {
                nextSink({(const char *) outbuf, sizeof(outbuf) - strm.avail_out});
                strm.next_out = (char *) outbuf;
                strm.avail_out = sizeof(outbuf);
            }
        }
    }
};

struct BrotliCompressionSink : ChunkedCompressionSink
{
    Sink & nextSink;
    uint8_t outbuf[BUFSIZ];
    BrotliEncoderState *state;
    bool finished = false;

    BrotliCompressionSink(Sink & nextSink) : nextSink(nextSink)
    {
        state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        if (!state)
            throw CompressionError("unable to initialise brotli encoder");
    }

    ~BrotliCompressionSink()
    {
        BrotliEncoderDestroyInstance(state);
    }

    void finish() override
    {
        flush();
        writeInternal({});
    }

    void writeInternal(std::string_view data) override
    {
        auto next_in = (const uint8_t *) data.data();
        size_t avail_in = data.size();
        uint8_t * next_out = outbuf;
        size_t avail_out = sizeof(outbuf);

        while (!finished && (!data.data() || avail_in)) {
            checkInterrupt();

            if (!BrotliEncoderCompressStream(state,
                    data.data() ? BROTLI_OPERATION_PROCESS : BROTLI_OPERATION_FINISH,
                    &avail_in, &next_in,
                    &avail_out, &next_out,
                    nullptr))
                throw CompressionError("error while compressing brotli compression");

            if (avail_out < sizeof(outbuf) || avail_in == 0) {
                nextSink({(const char *) outbuf, sizeof(outbuf) - avail_out});
                next_out = outbuf;
                avail_out = sizeof(outbuf);
            }

            finished = BrotliEncoderIsFinished(state);
        }
    }
};

ref<CompressionSink> makeCompressionSink(const std::string & method, Sink & nextSink, const bool parallel)
{
    if (method == "none")
        return make_ref<NoneSink>(nextSink);
    else if (method == "xz")
        return make_ref<XzCompressionSink>(nextSink, parallel);
    else if (method == "bzip2")
        return make_ref<BzipCompressionSink>(nextSink);
    else if (method == "br")
        return make_ref<BrotliCompressionSink>(nextSink);
    else
        throw UnknownCompressionMethod("unknown compression method '%s'", method);
}

ref<std::string> compress(const std::string & method, const std::string & in, const bool parallel)
{
    StringSink ssink;
    auto sink = makeCompressionSink(method, ssink, parallel);
    (*sink)(in);
    sink->finish();
    return ssink.s;
}

}
