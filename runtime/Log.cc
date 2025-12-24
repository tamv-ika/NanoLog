/* Copyright (c) 2017-2020 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <algorithm>
#include <chrono>

#include <bits/algorithmfwd.h>
#include <regex>
#include <vector>

#include "Log.h"
#include "GeneratedCode.h"

namespace NanoLogInternal
{

    /**
     * Friendly names for each #LogLevel value.
     * Keep this in sync with the LogLevel enum in NanoLog.h.
     */
    static const char *logLevelNames[] = {"(none)", "ERROR", "WARNING",
                                          "NOTICE", "DEBUG"};

    /**
     * Insert a checkpoint into an output buffer. This operation is fairly
     * expensive so it is typically performed once per new log file.
     *
     * \param out[in/out]
     *      Output array to insert the checkpoint into
     * \param outLimit
     *      Pointer to the end of out (i.e. first invalid byte to write to)
     *
     * \return
     *      True if operation succeed, false if there's not enough space
     */
    bool
    Log::insertCheckpoint(char **out, char *outLimit, bool writeDictionary)
    {
        if (static_cast<uint64_t>(outLimit - *out) < sizeof(Checkpoint))
            return false;

        Checkpoint *ck = reinterpret_cast<Checkpoint *>(*out);
        *out += sizeof(Checkpoint);

        ck->entryType = Log::EntryType::CHECKPOINT;
        ck->rdtsc = PerfUtils::Cycles::rdtsc();
        ck->unixTime = std::time(nullptr);
        ck->cyclesPerSecond = PerfUtils::Cycles::getCyclesPerSec();
        ck->newMetadataBytes = ck->totalMetadataEntries = 0;

        if (!writeDictionary)
            return true;

#ifdef PREPROCESSOR_NANOLOG
        long int bytesWritten = GeneratedFunctions::writeDictionary(*out, outLimit);

        if (bytesWritten == -1)
        {
            // roll back and exit
            *out -= sizeof(Checkpoint);
            return false;
        }

        *out += bytesWritten;
        ck->newMetadataBytes = static_cast<uint32_t>(bytesWritten);
        ck->totalMetadataEntries = static_cast<uint32_t>(
            GeneratedFunctions::numLogIds);
#endif // PREPROCESSOR_NANOLOG

        return true;
    }
    /**
     * Encoder constructor with dual buffers for realtime decompression support.
     * One buffer for log messages (BufferExtents), another for metadata
     * (Checkpoints + Dictionary).
     *
     * \param logBuffer
     *      Buffer to encode log messages (BufferExtents) to
     * \param logBufferSize
     *      The number of bytes usable within the log buffer
     * \param metadataBuffer
     *      Buffer to encode metadata (Checkpoints + Dictionary) to
     * \param metadataBufferSize
     *      The number of bytes usable within the metadata buffer
     * \param skipCheckpoint
     *      Optional parameter to skip embedding initial checkpoint.
     *      This parameter should never be set except in unit tests.
     * \param forceDictionaryOutput
     *      Force writing dictionary even in non-preprocessor mode
     */
    Log::Encoder::Encoder(char *logBuffer,
                          size_t logBufferSize,
                          char *metadataBuffer,
                          size_t metadataBufferSize,
                          bool skipCheckpoint,
                          bool forceDictionaryOutput)
        : backing_buffer(logBuffer), writePos(logBuffer), endOfBuffer(logBuffer + logBufferSize), metadataBacking(metadataBuffer), metadataWritePos(metadataBuffer), metadataEndOfBuffer(metadataBuffer + metadataBufferSize), currentCheckpointId(0), lastBufferIdEncoded(-1), currentExtentSize(nullptr), encodeMissDueToMetadata(0), consecutiveEncodeMissesDueToMetadata(0)
    {
        assert(logBuffer);
        assert(metadataBuffer);

        // Write initial checkpoint to metadata buffer
        if (skipCheckpoint && !forceDictionaryOutput)
            return;

#ifdef PREPROCESSOR_NANOLOG
        // For preprocessor mode, encode checkpoint + dictionary to metadata buffer
        // First, encode dictionary to measure its size
        char *dictStart = metadataWritePos + sizeof(Checkpoint);
        char *dictPos = dictStart;

        long bytesWritten = GeneratedFunctions::writeDictionary(dictPos, metadataEndOfBuffer);
        uint32_t dictionaryBytes = (bytesWritten > 0) ? static_cast<uint32_t>(bytesWritten) : 0;
        uint32_t totalEntries = static_cast<uint32_t>(GeneratedFunctions::numLogIds);

        // Now encode checkpoint with known dictionary size
        currentCheckpointId = encodeCheckpoint(dictionaryBytes, totalEntries);

        // Advance metadata write position past dictionary
        if (bytesWritten > 0)
        {
            metadataWritePos += bytesWritten;
        }
#else
        // For non-preprocessor mode, just write initial checkpoint
        // Dictionary will be added incrementally
        if (forceDictionaryOutput)
        {
            currentCheckpointId = encodeCheckpoint(0, 0);
        }
#endif
    }

    /**
     * Encode a checkpoint to the metadata buffer with current timestamp.
     *
     * \param dictionaryBytes
     *      Number of bytes of dictionary data that follows this checkpoint
     * \param totalEntries
     *      Total number of dictionary entries
     *
     * \return
     *      The checkpoint ID (timestamp in milliseconds)
     */
    uint64_t
    Log::Encoder::encodeCheckpoint(uint32_t dictionaryBytes, uint32_t totalEntries)
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count();

        // Mask to 62 bits for Checkpoint's bitfield
        uint64_t checkpointId = static_cast<uint64_t>(millis) & 0x3FFFFFFFFFFFFFFFULL;

        if (sizeof(Checkpoint) > static_cast<size_t>(metadataEndOfBuffer - metadataWritePos))
        {
            fprintf(stderr, "NanoLog Error: Metadata buffer full, cannot write checkpoint\r\n");
            return currentCheckpointId;
        }

        Checkpoint *ck = reinterpret_cast<Checkpoint *>(metadataWritePos);
        ck->entryType = EntryType::CHECKPOINT;
        ck->checkpointId = checkpointId;
        ck->rdtsc = PerfUtils::Cycles::rdtsc();
        ck->unixTime = std::time(nullptr);
        ck->cyclesPerSecond = PerfUtils::Cycles::getCyclesPerSec();
        ck->newMetadataBytes = dictionaryBytes;
        ck->totalMetadataEntries = totalEntries;

        metadataWritePos += sizeof(Checkpoint);
        currentCheckpointId = checkpointId;

        return checkpointId;
    }

    /**
     * Get the number of bytes encoded in the metadata buffer.
     *
     * \return
     *      Number of bytes encoded in metadata buffer
     */
    size_t
    Log::Encoder::getMetadataEncodedBytes()
    {
        return metadataWritePos - metadataBacking;
    }

    /**
     * Releases the metadata buffer and replaces it with a different one.
     *
     * \param inBuffer
     *      the new buffer to swap in
     * \param inSize
     *      the amount of free space usable in the buffer
     * \param[out] outBuffer
     *      returns a pointer to the original buffer
     * \param[out] outLength
     *      returns the number of bytes of encoded data in the outBuffer
     * \param[out] outSize
     *      returns the original size of the buffer
     */
    void
    Log::Encoder::swapMetadataBuffer(char *inBuffer, size_t inSize,
                                     char **outBuffer, size_t *outLength,
                                     size_t *outSize)
    {
        char *ret = metadataBacking;
        size_t size = metadataWritePos - metadataBacking;
        size_t originalSize = metadataEndOfBuffer - metadataBacking;

        metadataBacking = inBuffer;
        metadataWritePos = inBuffer;
        metadataEndOfBuffer = inBuffer + inSize;

        if (outBuffer)
            *outBuffer = ret;

        if (outLength)
            *outLength = size;

        if (outSize)
            *outSize = originalSize;
    }

    // Static buffer for legacy single-buffer constructors
    static char legacyMetadataBuffer[64 * 1024];

    /**
     * Legacy constructor for backward compatibility.
     * Uses internal static metadata buffer.
     *
     * \param buffer
     *      Buffer to store the encoded data in
     * \param size
     *      The number of bytes usable within the buffer
     * \param skipCheckpoint
     *      Optional parameter to skip embedding initial checkpoint.
     * \param forceDictionaryOutput
     *      Force writing dictionary
     */
    Log::Encoder::Encoder(char *buffer, size_t size, bool skipCheckpoint,
                          bool forceDictionaryOutput)
        : Encoder(buffer, size, legacyMetadataBuffer, sizeof(legacyMetadataBuffer),
                  skipCheckpoint, forceDictionaryOutput)
    {
    }

    /**
     * Given a vector of StaticLogInfo and a starting index, encode all the static
     * log information into a partial dictionary in the METADATA buffer.
     * Also encodes a new checkpoint before the dictionary.
     *
     * \param[in/out] currentPosition
     *      Starting/Ending index
     * \param allMetadata
     *      All the static log information encountered so far
     *
     * \return
     *      Number of bytes encoded in the dictionary (including DictionaryFragment header)
     */
    uint32_t
    Log::Encoder::encodeNewDictionaryEntries(uint32_t &currentPosition,
                                             std::vector<StaticLogInfo> allMetadata)
    {
        if (currentPosition >= allMetadata.size())
            return 0;

        // First, encode dictionary entries to a temporary position to measure size
        char *dictStart = metadataWritePos;

        if (sizeof(DictionaryFragment) >=
            static_cast<size_t>(metadataEndOfBuffer - metadataWritePos))
            return 0;

        DictionaryFragment *df = reinterpret_cast<DictionaryFragment *>(metadataWritePos);
        metadataWritePos += sizeof(DictionaryFragment);
        df->entryType = EntryType::LOG_MSGS_OR_DIC;

        uint32_t startPosition = currentPosition;

        while (currentPosition < allMetadata.size())
        {
            StaticLogInfo &curr = allMetadata.at(currentPosition);
            size_t filenameLength = strlen(curr.filename) + 1;
            size_t formatLength = strlen(curr.formatString) + 1;
            size_t nextDictSize = sizeof(CompressedLogInfo) + filenameLength + formatLength;

            // Not enough space, break out!
            if (nextDictSize >= static_cast<size_t>(metadataEndOfBuffer - metadataWritePos))
                break;

            CompressedLogInfo *cli = reinterpret_cast<CompressedLogInfo *>(metadataWritePos);
            metadataWritePos += sizeof(CompressedLogInfo);

            cli->severity = curr.severity;
            cli->linenum = curr.lineNum;
            cli->filenameLength = static_cast<uint16_t>(filenameLength);
            cli->formatStringLength = static_cast<uint16_t>(formatLength);

            memcpy(metadataWritePos, curr.filename, filenameLength);
            memcpy(metadataWritePos + filenameLength, curr.formatString, formatLength);
            metadataWritePos += filenameLength + formatLength;
            ++currentPosition;
        }

        uint32_t dictBytes = static_cast<uint32_t>(metadataWritePos - dictStart);
        df->newMetadataBytes = 0x3FFFFFFF & dictBytes;
        df->totalMetadataEntries = currentPosition;

        // If we encoded any new entries, update checkpoint with new ID
        if (currentPosition > startPosition)
        {
            // We need to insert checkpoint BEFORE the dictionary
            // Move dictionary data to make room for checkpoint
            size_t dictDataSize = metadataWritePos - dictStart;

            // Check if we have room for checkpoint
            if (sizeof(Checkpoint) + dictDataSize >
                static_cast<size_t>(metadataEndOfBuffer - dictStart))
            {
                // Not enough room, revert
                metadataWritePos = dictStart;
                currentPosition = startPosition;
                return 0;
            }

            // Move dictionary forward to make room for checkpoint
            memmove(dictStart + sizeof(Checkpoint), dictStart, dictDataSize);
            metadataWritePos += sizeof(Checkpoint);

            // Write checkpoint at dictStart
            using namespace std::chrono;
            auto now = system_clock::now();
            auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count();
            currentCheckpointId = static_cast<uint64_t>(millis) & 0x3FFFFFFFFFFFFFFFULL;

            Checkpoint *ck = reinterpret_cast<Checkpoint *>(dictStart);
            ck->entryType = EntryType::CHECKPOINT;
            ck->checkpointId = currentCheckpointId;
            ck->rdtsc = PerfUtils::Cycles::rdtsc();
            ck->unixTime = std::time(nullptr);
            ck->cyclesPerSecond = PerfUtils::Cycles::getCyclesPerSec();
            ck->newMetadataBytes = dictBytes;
            ck->totalMetadataEntries = currentPosition;

            return dictBytes;
        }

        return 0;
    }

#ifdef PREPROCESSOR_NANOLOG
    /**
     * Interprets the uncompressed log messages (created by the compile-time
     * generated code) contained in the *from buffer and compresses them to
     * the internal buffer. The encoded data can later be retrieved via swapBuffer()
     *
     * \param from
     *      A buffer containing the uncompressed log message created by the
     *      compile-time generated code
     * \param nbytes
     *      Maximum number of bytes that can be extracted from the *from buffer
     * \param bufferId
     *      The runtime thread/StagingBuffer id to associate the logs with
     * \param newPass
     *      Indicates that this encoding correlates with starting a new pass
     *      through the runtime StagingBuffers. In other words, this should be true
     *      on the first invocation of this function after the runtime has checked
     *      and encoded all the StagingBuffers at least once.
     * \param[out] numEventsCompressed
     *      adds the number of log messages processed in this invocation
     *
     * \return
     *      The number of bytes read from *from. A value of 0 indicates there is
     *      insufficient space in the internal buffer to fit the compressed message.
     */
    long
    Log::Encoder::encodeLogMsgs(char *from,
                                uint64_t nbytes,
                                uint32_t bufferId,
                                bool newPass,
                                uint64_t *numEventsCompressed,
                                uint64_t checkpointId)
    {
        if (!encodeBufferExtentStart(bufferId, newPass, checkpointId))
            return 0;

        uint64_t lastTimestamp = 0;
        long remaining = nbytes;
        long numEventsProcessed = 0;
        char *bufferStart = writePos;

        while (remaining > 0)
        {
            auto *entry = reinterpret_cast<UncompressedEntry *>(from);

            if (entry->entrySize > remaining)
            {
                if (entry->entrySize < (NanoLogConfig::STAGING_BUFFER_SIZE / 2))
                    break;

                GeneratedFunctions::LogMetadata &lm = GeneratedFunctions::logId2Metadata[entry->fmtId];
                fprintf(stderr, "ERROR: Attempting to log a message that is %u "
                                "bytes while the maximum allowable size is %u.\r\n"
                                "This occurs for the log message %s:%u '%s'\r\n",
                        entry->entrySize,
                        NanoLogConfig::STAGING_BUFFER_SIZE / 2,
                        lm.fileName, lm.lineNumber, lm.fmtString);
            }

            // Check for free space using the worst case assumption that
            // none of the arguments compressed and there are as many Nibbles
            // as there are data bytes.
            uint32_t maxCompressedSize = downCast<uint32_t>(2 * entry->entrySize + sizeof(Log::UncompressedEntry));
            if (maxCompressedSize > (endOfBuffer - writePos))
                break;

            compressLogHeader(entry, &writePos, lastTimestamp);
            lastTimestamp = entry->timestamp;

            size_t argBytesWritten =
                GeneratedFunctions::compressFnArray[entry->fmtId](entry, writePos);
            writePos += argBytesWritten;

            remaining -= entry->entrySize;
            from += entry->entrySize;

            ++numEventsProcessed;
        }

        assert(currentExtentSize);
        uint32_t currentSize;
        std::memcpy(&currentSize, currentExtentSize, sizeof(uint32_t));
        currentSize += downCast<uint32_t>(writePos - bufferStart);
        std::memcpy(currentExtentSize, &currentSize, sizeof(uint32_t));

        if (numEventsCompressed)
            *numEventsCompressed += numEventsProcessed;

        return nbytes - remaining;
    }
#endif // PREPROCESSOR_NANOLOG

    /**
     * Compresses a *from buffer filled with UncompressedEntry's and their
     * arguments to an internal buffer. The encoded data can then later be retrieved
     * via swapBuffer().
     *
     * This function is specialized for the non-preprocessor version of NanoLog and
     * requires a dictionary mapping logId's to static log information to be
     * explicitly passed in.
     *
     * \param from
     *      A buffer containing the uncompressed log message created by the
     *      the non-preprocessor version of NanoLog
     * \param nbytes
     *      Maximum number of bytes that can be extracted from the *from buffer
     * \param bufferId
     *      The runtime thread/StagingBuffer id to associate the logs with
     * \param newPass
     *      Indicates that this encoding correlates with starting a new pass
     *      through the runtime StagingBuffers. In other words, this should be true
     *      on the first invocation of this function after the runtime has checked
     *      and encoded all the StagingBuffers at least once.
     * \param[out] numEventsCompressed
     *      adds the number of log messages processed in this invocation
     *
     * \return
     *      The number of bytes read from *from. A value of 0 indicates there is
     *      insufficient space in the internal buffer to fit the compressed message.
     */
    long
    Log::Encoder::encodeLogMsgs(char *from,
                                uint64_t nbytes,
                                uint32_t bufferId,
                                bool newPass,
                                std::vector<StaticLogInfo> dictionary,
                                uint64_t *numEventsCompressed,
                                uint64_t checkpointId)
    {
        if (!encodeBufferExtentStart(bufferId, newPass, checkpointId))
            return 0;

        uint64_t lastTimestamp = 0;
        long remaining = nbytes;
        long numEventsProcessed = 0;
        char *bufferStart = writePos;

        while (remaining > 0)
        {
            auto *entry = reinterpret_cast<UncompressedEntry *>(from);

            // New log entry that we have not observed yet
            if (dictionary.size() <= entry->fmtId)
            {
                ++encodeMissDueToMetadata;
                ++consecutiveEncodeMissesDueToMetadata;

                // If we miss a whole bunch, then we start printing out errors
                if (consecutiveEncodeMissesDueToMetadata % 1000 == 0)
                {
                    fprintf(stderr, "NanoLog Error: Metadata missing for a dynamic "
                                    "log message (id=%u) during compression. If "
                                    "you are using Preprocessor NanoLog, there is "
                                    "be a problem with your integration (static "
                                    "logs detected=%lu).\r\n",
                            entry->fmtId,
                            GeneratedFunctions::numLogIds);
                }

                break;
            }

            consecutiveEncodeMissesDueToMetadata = 0;

#ifdef ENABLE_DEBUG_PRINTING
            printf("Trying to encode fmtId=%u, size=%u, remaining=%ld\r\n",
                   entry->fmtId, entry->entrySize, remaining);
            printf("\t%s\r\n", dictionary.at(entry->fmtId).formatString);
#endif

            if (entry->entrySize > remaining)
            {
                if (entry->entrySize < (NanoLogConfig::STAGING_BUFFER_SIZE / 2))
                    break;

                StaticLogInfo &info = dictionary.at(entry->fmtId);
                fprintf(stderr, "NanoLog ERROR: Attempting to log a message that "
                                "is %u bytes while the maximum allowable size is "
                                "%u.\r\n This occurs for the log message %s:%u '%s'"
                                "\r\n",
                        entry->entrySize,
                        NanoLogConfig::STAGING_BUFFER_SIZE / 2,
                        info.filename, info.lineNum, info.formatString);
            }

            // Check for free space using the worst case assumption that
            // none of the arguments compressed and there are as many Nibbles
            // as there are data bytes.
            uint32_t maxCompressedSize = downCast<uint32_t>(2 * entry->entrySize + sizeof(Log::UncompressedEntry));
            if (maxCompressedSize > (endOfBuffer - writePos))
                break;

            compressLogHeader(entry, &writePos, lastTimestamp);
            lastTimestamp = entry->timestamp;

            StaticLogInfo &info = dictionary.at(entry->fmtId);
#ifdef ENABLE_DEBUG_PRINTING
            printf("\r\nCompressing \'%s\' with info.id=%d\r\n",
                   info.formatString, entry->fmtId);
#endif
            char *argData = entry->argData;
            info.compressionFunction(info.numNibbles, info.paramTypes,
                                     &argData, &writePos);

            remaining -= entry->entrySize;
            from += entry->entrySize;

            ++numEventsProcessed;
        }

        assert(currentExtentSize);
        uint32_t currentSize;
        std::memcpy(&currentSize, currentExtentSize, sizeof(uint32_t));
        currentSize += downCast<uint32_t>(writePos - bufferStart);
        std::memcpy(currentExtentSize, &currentSize, sizeof(uint32_t));

        if (numEventsCompressed)
            *numEventsCompressed += numEventsProcessed;

        return nbytes - remaining;
    }

    /**
     * Internal function that encodes a marker indicating that all log messages
     * after this point (but after the next marker) belong to a particular buffer.
     * This is only used in encodeLogMsgs, but is separated out to allow for easy
     * unit testing with its counterpart in Decoder.
     *
     * \param bufferId
     *      Buffer id to encode in the extent
     * \param newPass
     *      Indicates whether this buffer change also correlates with the start of
     *      a new pass through the runtime StagingBuffers by the caller
     * \param checkpointId
     *      The checkpoint ID to associate with this buffer extent for realtime
     *      decompression correlation
     * \return
     *      Whether the operation completed successfully (true) or failed due to
     *      lack of space in the internal buffer (false)
     */
    bool
    Log::Encoder::encodeBufferExtentStart(uint32_t bufferId, bool newPass,
                                          uint64_t checkpointId)
    {
        // For size check, assume the worst case of no compression on bufferId
        char *writePosStart = writePos;
        if (sizeof(BufferExtent) + sizeof(bufferId) >
            static_cast<size_t>(endOfBuffer - writePos))
            return false;

        BufferExtent *tc = reinterpret_cast<BufferExtent *>(writePos);
        writePos += sizeof(BufferExtent);

        tc->entryType = EntryType::BUFFER_EXTENT;
        tc->wrapAround = newPass;
        tc->checkpointId = checkpointId;

        if (bufferId < (1 << 4))
        {
            tc->isShort = true;
            tc->threadIdOrPackNibble = 0x0F & bufferId;
        }
        else
        {
            tc->isShort = false;
            tc->threadIdOrPackNibble = 0x0F & BufferUtils::pack<uint32_t>(
                                                  &writePos, bufferId);
        }

        tc->length = downCast<uint32_t>(writePos - writePosStart);
        currentExtentSize = &(tc->length);
        lastBufferIdEncoded = bufferId;

        return true;
    }

    /**
     * Retrieve the number of bytes encoded in the internal buffer
     *
     * \return
     *      Number of bytes encoded in the internal buffer
     */
    size_t
    Log::Encoder::getEncodedBytes()
    {
        return writePos - backing_buffer;
    }

    /**
     * Releases the internal buffer and replaces it with a different one.
     *
     * \param inBuffer
     *      the new buffer to swap in
     * \param inSize
     *      the amount of free space usable in the buffer
     * \param[out] outBuffer
     *      returns a pointer to the original buffer
     * \param[out] outLength
     *      returns the number of bytes of encoded data in the outBuffer
     */
    void
    Log::Encoder::swapBuffer(char *inBuffer, size_t inSize, char **outBuffer,
                             size_t *outLength, size_t *outSize)
    {
        char *ret = backing_buffer;
        size_t size = writePos - backing_buffer;
        size_t originalSize = endOfBuffer - backing_buffer;

        backing_buffer = inBuffer;
        writePos = inBuffer;
        endOfBuffer = inBuffer + inSize;
        lastBufferIdEncoded = -1;
        currentExtentSize = nullptr;

        if (outBuffer)
            *outBuffer = ret;

        if (outLength)
            *outLength = size;

        if (outSize)
            *outSize = originalSize;
    }

    // Constructor for LogMessage
    Log::LogMessage::LogMessage()
        : metadata(nullptr), logId(-1), rdtsc(0), numArgs(0), totalCapacity(sizeof(rawArgs) / sizeof(uint64_t)), rawArgs(), rawArgsExtension(nullptr)
    {
    }

    // Destructor for LogMessage
    Log::LogMessage::~LogMessage()
    {
        if (rawArgsExtension != nullptr)
        {
            free(rawArgsExtension);
            rawArgsExtension = nullptr;
            totalCapacity = INITIAL_SIZE;
        }
    }

    /**
     * Reserve/Allocate enough space for N parameters in the structure
     *
     * \param nparams
     *    Number of parameters to ensure space for
     */
    void
    Log::LogMessage::reserve(int nparams)
    {
        if (totalCapacity >= nparams)
            return;

        while (totalCapacity < nparams)
            totalCapacity *= 2;

        size_t newSize = sizeof(uint64_t) * (totalCapacity - INITIAL_SIZE);
        uint64_t *newAllocation = static_cast<uint64_t *>(malloc(newSize));

        if (newAllocation == nullptr)
        {
            fprintf(stderr, "Could not allocate memory to store %d log "
                            "arguments. Exiting...",
                    nparams);
            exit(1);
        }

        if (rawArgsExtension == nullptr)
        {
            rawArgsExtension = newAllocation;
            return;
        }

        memcpy(newAllocation,
               rawArgsExtension,
               sizeof(uint64_t) * (numArgs - INITIAL_SIZE));
        free(rawArgsExtension);

        rawArgsExtension = newAllocation;
    }

    /**
     * Ready's the structure for a new log statement by reassigning the static
     * log information. Dynamic arguments can then be push()-ed into the
     * structure.
     *
     * \param meta
     *      Static log data to refer to
     * \param logId
     *      Preprocessor assigned log id of the log message
     * \param rdtsc
     *      Invocation time of the log message
     */
    void
    Log::LogMessage::reset(FormatMetadata *meta, uint32_t logId, uint64_t rdtsc)
    {
        this->metadata = meta;
        this->rdtsc = rdtsc;
        this->logId = logId;
        numArgs = 0;
    }

    // Indicates whether the structure stores a valid log statement or not
    bool Log::LogMessage::valid()
    {
        return metadata != nullptr;
    }

    // Returns the number of arguments currently stored for the log.
    int Log::LogMessage::getNumArgs()
    {
        return numArgs;
    }

    // Returns the preprocessor-assigned log identifier for this log message
    uint32_t Log::LogMessage::getLogId()
    {
        return logId;
    }

    // Returns the runtime timestamp for the log invocation.
    uint64_t Log::LogMessage::getTimestamp()
    {
        return rdtsc;
    }

    /**
     * Decoder constructor.
     *
     * Due to the large amount of memory needed to buffer log statements, the
     * decoder is intended to be constructed once and then re-used via open().
     */
    Log::Decoder::Decoder()
        : filename(), inputFd(nullptr), metadataFd(nullptr), useSeparateMetadata(false), logMsgsPrinted(0), bufferFragment(nullptr), good(false), checkpoint(), freeBuffers(), fmtId2metadata(), fmtId2fmtString(), rawMetadata(nullptr), endOfRawMetadata(nullptr), numBufferFragmentsRead(0), numCheckpointsRead(0)
    {
        // Take advantage of virtual memory an allocate an insanely large (1GB)
        // buffer to store log metadata read from the logFile. Such a large buffer
        // is used so that we don't have to explicitly manage the buffer and instead
        // leave it up to the virtual memory system.
        rawMetadata = static_cast<char *>(malloc(1024 * 1024 * 1024));

        if (rawMetadata == nullptr)
        {
            fprintf(stderr, "Could not allocate an internal 1GB buffer to store log"
                            " metadata");
            exit(-1);
        }

        endOfRawMetadata = rawMetadata;
        fmtId2metadata.reserve(1000);
        fmtId2fmtString.reserve(1000);
        bufferFragment = allocateBufferFragment();
    }

    /**
     * Reads the metadata necessary to decompress log messages from a log file.
     * This function can be invoked incrementally to build a larger dictionary from
     * smaller fragments in the file and it should only be invoked once per fragment
     *
     * \param fd
     *      File descriptor pointing to the dictionary fragment
     * \param flushOldDictionary
     *      Removes the old dictionary entries
     * \return
     *      true if successful, false if the dictionary was corrupt
     */
    bool
    Log::Decoder::readDictionary(FILE *fd, bool flushOldDictionary)
    {
        if (!readCheckpoint(checkpoint, fd))
        {
            fprintf(stderr, "Error: Could not read initial checkpoint, "
                            "the compressed log may be corrupted.\r\n");
            return false;
        }

        size_t bytesRead = fread(endOfRawMetadata, 1, checkpoint.newMetadataBytes,
                                 fd);
        if (bytesRead != checkpoint.newMetadataBytes)
        {
            fprintf(stderr, "Error couldn't read metadata header in log file.\r\n");
            return false;
        }

        if (flushOldDictionary)
        {
            endOfRawMetadata = rawMetadata;
            fmtId2metadata.clear();
            fmtId2fmtString.clear();
        }

        // Build an index of format id to metadata
        const char *start = endOfRawMetadata;
        const char *newEnd = endOfRawMetadata + bytesRead;
        while (endOfRawMetadata < newEnd)
        {
            std::string fmtString;
            fmtId2metadata.push_back(endOfRawMetadata);

            // Skip ahead
            auto *fm = reinterpret_cast<FormatMetadata *>(endOfRawMetadata);
            endOfRawMetadata += sizeof(FormatMetadata) + fm->filenameLength;

            for (int i = 0; i < fm->numPrintFragments && newEnd >= endOfRawMetadata; ++i)
            {
                auto *pf = reinterpret_cast<PrintFragment *>(endOfRawMetadata);
                endOfRawMetadata += sizeof(PrintFragment) + pf->fragmentLength;
                fmtString.append(pf->formatFragment);
            }

            fmtId2fmtString.push_back(fmtString);
        }

        if (newEnd != endOfRawMetadata)
        {
            fprintf(stderr, "Error: Log dictionary is inconsistent; "
                            "expected %lu bytes, but read %lu bytes\r\n",
                    newEnd - start,
                    endOfRawMetadata - start);
            return false;
        }

        if (fmtId2metadata.size() != checkpoint.totalMetadataEntries)
        {
            fprintf(stderr, "Error: Missing log metadata detected; "
                            "expected %u messages, but only found %lu\r\n",
                    checkpoint.totalMetadataEntries,
                    fmtId2metadata.size());
            return false;
        }

        ++numCheckpointsRead;
        return true;
    }

    /**
     * Parses the <length> and <specifier> components of a printf format sub-string
     * according to http://www.cplusplus.com/reference/cstdio/printf/ and returns
     * a corresponding FormatType.
     *
     * \param length
     *      Length component of the printf format string
     * \param specifier
     *      Specifier component of the printf format string
     * @return
     *      The FormatType corresponding to the length and specifier. A value of
     *      MAX_FORMAT_TYPE is returned in case of error.
     */
    static NanoLogInternal::Log::FormatType
    getFormatType(std::string length, char specifier)
    {
        using namespace NanoLogInternal::Log;

        // Signed Integers
        if (specifier == 'd' || specifier == 'i')
        {
            if (length.empty())
                return int_t;

            if (length.size() == 2)
            {
                if (length[0] == 'h')
                    return signed_char_t;
                if (length[0] == 'l')
                    return long_long_int_t;
            }

            switch (length[0])
            {
            case 'h':
                return short_int_t;
            case 'l':
                return long_int_t;
            case 'j':
                return intmax_t_t;
            case 'z':
                return size_t_t;
            case 't':
                return ptrdiff_t_t;
            default:
                break;
            }
        }

        // Unsigned integers
        if (specifier == 'u' || specifier == 'o' || specifier == 'x' || specifier == 'X')
        {
            if (length.empty())
                return unsigned_int_t;

            if (length.size() == 2)
            {
                if (length[0] == 'h')
                    return unsigned_char_t;
                if (length[0] == 'l')
                    return unsigned_long_long_int_t;
            }

            switch (length[0])
            {
            case 'h':
                return unsigned_short_int_t;
            case 'l':
                return unsigned_long_int_t;
            case 'j':
                return uintmax_t_t;
            case 'z':
                return size_t_t;
            case 't':
                return ptrdiff_t_t;
            default:
                break;
            }
        }

        // Strings
        if (specifier == 's')
        {
            if (length.empty())
                return const_char_ptr_t;
            if (length[0] == 'l')
                return const_wchar_t_ptr_t;
        }

        // Pointer
        if (specifier == 'p')
        {
            if (length.empty())
                return const_void_ptr_t;
        }

        // Floating points
        if (specifier == 'f' || specifier == 'F' || specifier == 'e' || specifier == 'E' || specifier == 'g' || specifier == 'G' || specifier == 'a' || specifier == 'A')
        {
            if (length.size() == 1 && length[0] == 'L')
                return long_double_t;
            else
                return double_t;
        }

        if (specifier == 'c')
        {
            if (length.empty())
                return int_t;
            if (length[0] == 'l')
                return wint_t_t;
        }

        fprintf(stderr, "Attempt to decode format specifier failed: %s%c\r\n",
                length.c_str(), specifier);
        return MAX_FORMAT_TYPE;
    }

    /**
     * Generate a more efficient internal representation describing how to process
     * the compressed arguments of a NANO_LOG statement given its static
     * log information.
     *
     * The output representation would look something like a FormatMetadata with
     * nested PrintFragments. This representation can then be interpreted by the
     * state machine in decompressNextLogStatement to decompress the log statements.
     *
     * \param[in/out] microCode
     *      Buffer to store the generated internal representation
     * \param formatString
     *      Format string of the NANO_LOG statement
     * \param filename
     *      File associated with the log invocation site
     * \param linenum
     *      Line number within filename associated with the log invocation site
     * \param severity
     *      LogLevel severity associated with the log invocation site
     * \return
     *      true indicates success; false indicates malformed printf format string
     */
    bool
    Log::Decoder::createMicroCode(char **microCode,
                                  const char *formatString,
                                  const char *filename,
                                  uint32_t linenum,
                                  uint8_t severity)
    {
        using namespace NanoLogInternal::Log;

        size_t formatStringLength = strlen(formatString) + 1; // +1 for NULL
        char *microCodeStartingPos = *microCode;
        FormatMetadata *fm = reinterpret_cast<FormatMetadata *>(*microCode);
        *microCode += sizeof(FormatMetadata);

        fm->logLevel = severity;
        fm->lineNumber = linenum;
        fm->filenameLength = static_cast<uint16_t>(strlen(filename) + 1);
        *microCode = stpcpy(*microCode, filename) + 1;

        fm->numNibbles = 0;
        fm->numPrintFragments = 0;

        std::regex regex("^%"
                         "([-+ #0]+)?"            // Flags (Position 1)
                         "([\\d]+|\\*)?"          // Width (Position 2)
                         "(\\.(\\d+|\\*))?"       // Precision (Position 4; 3 includes '.')
                         "(hh|h|l|ll|j|z|Z|t|L)?" // Length (Position 5)
                         "([diuoxXfFeEgGaAcspn])" // Specifier (Position 6)
        );

        size_t i = 0;
        std::cmatch match;
        int consecutivePercents = 0;
        size_t startOfNextFragment = 0;
        PrintFragment *pf = nullptr;

        // The key idea here is to split up the format string in to fragments (i.e.
        // PrintFragments) such that there is at most one specifier per fragment.
        // This then allows the decompressor later to consume one argument at a
        // time and print the fragment (vs. buffering all the arguments first).

        while (i < formatStringLength)
        {
            char c = formatString[i];

            // Skip the next character if there's an escape
            if (c == '\\')
            {
                i += 2;
                continue;
            }

            if (c != '%')
            {
                ++i;
                consecutivePercents = 0;
                continue;
            }

            // If there's an even number of '%'s, then it's a comment
            if (++consecutivePercents % 2 == 0 || !std::regex_search(formatString + i, match, regex))
            {
                ++i;
                continue;
            }

            // Advance the pointer to the end of the specifier & reset the % counter
            consecutivePercents = 0;
            i += match.length();

            // At this point we found a match, let's start analyzing it
            pf = reinterpret_cast<PrintFragment *>(*microCode);
            *microCode += sizeof(PrintFragment);

            std::string width = match[2].str();
            std::string precision = match[4].str();
            std::string length = match[5].str();
            char specifier = match[6].str()[0];

            FormatType type = getFormatType(length, specifier);
            if (type == MAX_FORMAT_TYPE)
            {
                fprintf(stderr, "Error: Couldn't process this: %s\r\n",
                        match.str().c_str());
                *microCode = microCodeStartingPos;
                return false;
            }

            pf->argType = 0x1F & type;
            pf->hasDynamicWidth = (width.empty()) ? false : width[0] == '*';
            pf->hasDynamicPrecision = (precision.empty()) ? false
                                                          : precision[0] == '*';

            // Tricky tricky: We null-terminate the fragment by copying 1
            // extra byte and then setting it to NULL
            pf->fragmentLength = static_cast<uint16_t>(i - startOfNextFragment + 1);
            memcpy(*microCode,
                   formatString + startOfNextFragment,
                   pf->fragmentLength);
            *microCode += pf->fragmentLength;
            *(*microCode - 1) = '\0';

            // Non-strings and dynamic widths need nibbles!
            if (specifier != 's')
                ++fm->numNibbles;

            if (pf->hasDynamicWidth)
                ++fm->numNibbles;

            if (pf->hasDynamicPrecision)
                ++fm->numNibbles;

#ifdef ENABLE_DEBUG_PRINTING
            printf("Fragment %d: %s\r\n", fm->numPrintFragments, pf->formatFragment);
            printf("\t\ttype: %u, dWidth: %u, dPrecision: %u, length: %u\r\n",
                   pf->argType,
                   pf->hasDynamicWidth,
                   pf->hasDynamicPrecision,
                   pf->fragmentLength);
#endif

            startOfNextFragment = i;
            ++fm->numPrintFragments;
        }

        // If we didn't encounter any specifiers, make one for a basic string
        if (pf == nullptr)
        {
            pf = reinterpret_cast<PrintFragment *>(*microCode);
            *microCode += sizeof(PrintFragment);
            fm->numPrintFragments = 1;

            pf->argType = FormatType::NONE;
            pf->hasDynamicWidth = pf->hasDynamicPrecision = false;
            pf->fragmentLength = downCast<uint16_t>(formatStringLength);
            memcpy(*microCode, formatString, formatStringLength);
            *microCode += formatStringLength;
        }
        else
        {
            // Extend the last fragment to include the rest of the string
            size_t endingLength = formatStringLength - startOfNextFragment;
            memcpy(pf->formatFragment + pf->fragmentLength - 1, // -1 to erase \0
                   formatString + startOfNextFragment,
                   endingLength);
            pf->fragmentLength = downCast<uint16_t>(pf->fragmentLength - 1 + endingLength);
            *microCode += endingLength;
        }

#ifdef ENABLE_DEBUG_PRINTING
        printf("Fragment %d: %s\r\n", fm->numPrintFragments, pf->formatFragment);
        printf("\t\ttype: %u, dWidth: %u, dPrecision: %u, length: %u\r\n",
               pf->argType,
               pf->hasDynamicWidth,
               pf->hasDynamicPrecision,
               pf->fragmentLength);
#endif

        return true;
    }

    /**
     * Reads a partial dictionary from the log file and adds it to the global
     * mapping of log identifiers to static log information.
     *
     * \param fd
     *      File descriptor to read the mapping from
     * \return
     *      true indicates success; false indicates error
     */
    bool
    Log::Decoder::readDictionaryFragment(FILE *fd)
    {
        // These buffers start us off with some statically allocated space.
        // Should we need more, we will malloc it.
        size_t bufferSize = 10 * 1024;
        char filenameBuffer[bufferSize];
        char formatBuffer[bufferSize];

        bool newBuffersAllocated = false;
        char *filename = filenameBuffer;
        char *format = formatBuffer;

        DictionaryFragment df;
        size_t bytesRead = fread(&df, 1, sizeof(DictionaryFragment), fd);
        if (bytesRead != sizeof(DictionaryFragment))
        {
            fprintf(stderr, "Could not read entire dictionary fragment header\r\n");
            return false;
        }

        assert(df.entryType == EntryType::LOG_MSGS_OR_DIC);

        while (bytesRead < df.newMetadataBytes && !feof(fd))
        {
            CompressedLogInfo cli;
            size_t newBytesRead = 0;
            newBytesRead += fread(&cli, 1, sizeof(CompressedLogInfo), fd);

            if (newBytesRead != sizeof(CompressedLogInfo))
            {
                fprintf(stderr, "Could not read in log metadata\r\n");
                return false;
            }

            if (bufferSize < cli.filenameLength ||
                bufferSize < cli.formatStringLength)
            {
                if (newBuffersAllocated)
                {
                    free(filename);
                    free(format);
                    filename = format = nullptr;
                }

                newBuffersAllocated = true;
                bufferSize = 2 * std::max(cli.filenameLength, cli.formatStringLength);
                filename = static_cast<char *>(malloc(bufferSize));
                format = static_cast<char *>(malloc(bufferSize));

                if (filename == nullptr || format == nullptr)
                {
                    fprintf(stderr, "Internal Error: Could not allocate enough "
                                    "memory to store the filename/format strings "
                                    "in the dictionary. Tried to allocate %lu bytes "
                                    "to store the %u and %u byte filename and "
                                    "format string lengths respectively",
                            bufferSize,
                            cli.formatStringLength,
                            cli.filenameLength);
                    return false;
                }
            }

            newBytesRead += fread(filename, 1, cli.filenameLength, fd);
            newBytesRead += fread(format, 1, cli.formatStringLength, fd);
            bytesRead += newBytesRead;

            if (newBytesRead != sizeof(CompressedLogInfo) + cli.filenameLength + cli.formatStringLength)
            {
                fprintf(stderr, "Could not read in a log's filename/"
                                "format string\r\n");
                return false;
            }

            fmtId2metadata.push_back(endOfRawMetadata);
            fmtId2fmtString.push_back(format);
            createMicroCode(&endOfRawMetadata,
                            format,
                            filename,
                            cli.linenum,
                            cli.severity);
        }

        if (newBuffersAllocated)
        {
            free(filename);
            free(format);
        }

        return true;
    }

    /**
     * Opens a compressed log with contents created by Encoder.
     *
     * \param filename
     *      Compressed log file to open
     * \return
     *      True if success, false if the log file is not valid or cannot be opened
     */
    bool
    Log::Decoder::open(const char *filename)
    {
        inputFd = fopen(filename, "rb");
        good = false;

        if (!inputFd)
            return false;

        if (!readDictionary(inputFd, true))
        {
            fclose(inputFd);
            inputFd = nullptr;
            return false;
        }

        this->filename = std::string(filename);
        numBufferFragmentsRead = 0;
        numCheckpointsRead = 1;
        logMsgsPrinted = 0;
        good = true;
        useSeparateMetadata = false;
        return true;
    }

    /**
     * Opens a compressed log with separate metadata file (realtime mode).
     * The metadata file contains checkpoints and dictionary information,
     * while the log file contains only compressed log entries.
     *
     * \param logFilename
     *      Path to the compressed log file containing log entries
     * \param metadataFilename
     *      Path to the metadata file containing checkpoints and dictionary
     * \return
     *      True if success, false if files cannot be opened or are invalid
     */
    bool
    Log::Decoder::openWithSeparateMetadata(const char *logFilename,
                                           const char *metadataFilename)
    {
        // Open log file
        inputFd = fopen(logFilename, "rb");
        if (!inputFd)
        {
            fprintf(stderr, "Cannot open log file: %s\r\n", logFilename);
            return false;
        }

        // Open metadata file
        metadataFd = fopen(metadataFilename, "rb");
        if (!metadataFd)
        {
            fprintf(stderr, "Cannot open metadata file: %s\r\n", metadataFilename);
            fclose(inputFd);
            inputFd = nullptr;
            return false;
        }

        good = false;
        useSeparateMetadata = true;

        // Read metadata file
        if (!readMetadataFile(metadataFd))
        {
            fclose(inputFd);
            fclose(metadataFd);
            inputFd = nullptr;
            metadataFd = nullptr;
            useSeparateMetadata = false;
            return false;
        }

        this->filename = std::string(logFilename);
        numBufferFragmentsRead = 0;
        numCheckpointsRead = 1;
        logMsgsPrinted = 0;
        good = true;
        return true;
    }

    /**
     * Reads the entire metadata file, extracting all checkpoints and dictionary
     * information needed for decompression. The metadata file uses the same binary
     * format as the log file: Checkpoint entries followed by DictionaryFragment.
     *
     * \param metaFd
     *      File descriptor for the metadata file
     * \return
     *      True if successful, false if metadata file is corrupt
     */
    bool
    Log::Decoder::readMetadataFile(FILE *metaFd)
    {
        // Read all checkpoints and dictionary fragments
        endOfRawMetadata = rawMetadata;
        fmtId2metadata.clear();
        fmtId2fmtString.clear();
        checkpointMap.clear();

        while (!feof(metaFd))
        {
            if (!readMetadataCheckpoint(metaFd))
            {
                // End of file or error
                break;
            }
        }

        if (fmtId2metadata.empty() && checkpoint.totalMetadataEntries == 0)
        {
            // No dictionary yet - this is okay for realtime mode
            // The decompressor can wait for data
        }

        return true;
    }

    /**
     * Reads a single checkpoint and its associated dictionary from the metadata
     * file. Uses the standard Checkpoint and DictionaryFragment types.
     *
     * \param metaFd
     *      File descriptor for the metadata file
     * \return
     *      True if successful, false if end of file or corrupt
     */
    bool
    Log::Decoder::readMetadataCheckpoint(FILE *metaFd)
    {
        // Read Checkpoint entry
        Checkpoint cp;
        size_t bytesRead = fread(&cp, 1, sizeof(Checkpoint), metaFd);

        if (bytesRead == 0)
        {
            // End of file
            return false;
        }

        if (bytesRead != sizeof(Checkpoint))
        {
            fprintf(stderr, "Error: Incomplete checkpoint\r\n");
            return false;
        }

        if (cp.entryType != EntryType::CHECKPOINT)
        {
            fprintf(stderr, "Error: Expected checkpoint, got entry type %u\r\n",
                    cp.entryType);
            return false;
        }

        // Store checkpoint with its ID for lookup during decompression
        checkpoint = cp;
        checkpointMap[cp.checkpointId] = cp;

        // Read dictionary data if present
        if (cp.newMetadataBytes > 0)
        {
            // Read DictionaryFragment header
            DictionaryFragment df;
            bytesRead = fread(&df, 1, sizeof(DictionaryFragment), metaFd);
            if (bytesRead != sizeof(DictionaryFragment))
            {
                fprintf(stderr, "Error: Could not read dictionary fragment header\r\n");
                return false;
            }

            if (df.entryType != EntryType::LOG_MSGS_OR_DIC)
            {
                fprintf(stderr, "Error: Expected dictionary fragment, got entry type %u\r\n",
                        df.entryType);
                return false;
            }

            // df.newMetadataBytes includes the DictionaryFragment header size
            // so dictionary entry data size = total - header
            size_t dictDataSize = df.newMetadataBytes - sizeof(DictionaryFragment);
            
            if (dictDataSize > 0) {
                // Read dictionary data into a SEPARATE temporary buffer
                // because createMicroCode will write into endOfRawMetadata,
                // and we don't want to overwrite unread dictionary entries
                std::vector<char> tempBuffer(dictDataSize);

                bytesRead = fread(tempBuffer.data(), 1, dictDataSize, metaFd);
                if (bytesRead != dictDataSize)
                {
                    fprintf(stderr, "Error: Could not read dictionary data (expected %zu, got %zu)\r\n",
                            dictDataSize, bytesRead);
                    return false;
                }

                // Parse dictionary entries from temporary buffer
                const char *readPos = tempBuffer.data();
                const char *dictEnd = tempBuffer.data() + dictDataSize;

            while (readPos < dictEnd)
            {
                CompressedLogInfo *cli =
                    reinterpret_cast<CompressedLogInfo *>(
                        const_cast<char *>(readPos));
                readPos += sizeof(CompressedLogInfo);

                const char *fname = readPos;
                readPos += cli->filenameLength;

                const char *fmtStr = readPos;
                readPos += cli->formatStringLength;

                // Create microcode for this entry - writes to endOfRawMetadata
                fmtId2metadata.push_back(endOfRawMetadata);
                fmtId2fmtString.push_back(std::string(fmtStr));

                createMicroCode(&endOfRawMetadata,
                                fmtStr,
                                fname,
                                cli->linenum,
                                cli->severity);
            }
            }
        }

        ++numCheckpointsRead;
        return true;
    }

    /**
     * Decoder destructor
     */
    Log::Decoder::~Decoder()
    {
        if (inputFd)
            fclose(inputFd);

        if (metadataFd)
            fclose(metadataFd);

        filename.clear();
        inputFd = nullptr;
        metadataFd = nullptr;
        useSeparateMetadata = false;
        good = false;

        for (BufferFragment *bf : freeBuffers)
            delete bf;

        freeBuffers.clear();
    }

    /**
     * Allocates a BufferFragment to read BufferExtents into
     *
     * \return
     *      Allocated BufferFragment
     */
    Log::Decoder::BufferFragment *
    Log::Decoder::allocateBufferFragment()
    {
        if (!freeBuffers.empty())
        {
            BufferFragment *ret = freeBuffers.back();
            freeBuffers.pop_back();
            return ret;
        }

        return new BufferFragment();
    }

    /**
     * Internal function to store a BufferFragment on the free list
     *
     * \param bf
     *      BufferFragment to store
     */
    void
    Log::Decoder::freeBufferFragment(BufferFragment *bf)
    {
        bf->reset();
        freeBuffers.push_back(bf);
    }

    // BufferFragment constructor
    Log::Decoder::BufferFragment::BufferFragment()
        : storage(), validBytes(0), runtimeId(-1), checkpointId(0), readPos(nullptr), endOfBuffer(nullptr), hasMoreLogs(false), nextLogId(-1), nextLogTimestamp(0)
    {
    }

    /**
     * Resets the state of the BufferFragment so that the data cannot be reused
     */
    void
    Log::Decoder::BufferFragment::reset()
    {
        validBytes = 0;
        runtimeId = -1;
        checkpointId = 0;
        readPos = nullptr;
        endOfBuffer = nullptr;
        hasMoreLogs = false;
    }
    /**
     * Read in the next buffer fragment from the compressed log. If an error occurs
     * the file descriptor will be in an undefined state.
     *
     * \param fd
     *      File stream to read it from
     * \param[out] wrapAround
     *      Indicates whether a wrap around was indicated in the log or not.
     *
     * \return
     *      indicates whether the operation succeeded (true) or failed due to
     *      a malformed log data.
     */
    bool
    Log::Decoder::BufferFragment::readBufferExtent(FILE *fd, bool *wrapAround)
    {
        validBytes = fread(storage, 1, sizeof(BufferExtent), fd);
        BufferExtent *be = reinterpret_cast<BufferExtent *>(storage);

        if (be->entryType != EntryType::BUFFER_EXTENT ||
            validBytes < sizeof(BufferExtent) ||
            be->length > sizeof(storage))
        {
            reset();
            return false;
        }

        assert(be->length >= validBytes);
        uint64_t remaining = be->length - validBytes;
        validBytes += fread(storage + validBytes, 1, remaining, fd);

        if (validBytes != be->length)
        {
            reset();
            return false;
        }

        readPos = storage + sizeof(BufferExtent);
        endOfBuffer = storage + validBytes;

        if (be->isShort)
            runtimeId = be->threadIdOrPackNibble;
        else
            runtimeId = BufferUtils::unpack<uint32_t>(
                &readPos, be->threadIdOrPackNibble);

        // Store checkpoint ID for use in timestamp conversion
        checkpointId = be->checkpointId;

        if (wrapAround)
            *wrapAround = be->wrapAround;

        // The buffer has no log messages, skip it (this may be possible in cases
        // where we want to mark wrapArounds or the output buffer ran out of space).
        if (readPos == endOfBuffer)
        {
            hasMoreLogs = false;
            return true;
        }

        hasMoreLogs = decompressLogHeader(&readPos, 0, nextLogId, nextLogTimestamp);
        if (!hasMoreLogs)
            reset();

        return hasMoreLogs;
    }

    /**
     * Helper to decompressNextLogStatement to print a single PrintFragment
     * given an argument and optional width/precision specifiers.
     *
     * \tparam T
     *      Type of the argument (automatically inferred)
     * \param outputFd
     *      Where to output the statement
     * \param formatString
     *      Partial format string containing exactly 1 format specifier
     * \param arg
     *      Argument to pass in with the format string
     * \param width
     *      Width parameter of a printf-specifier, a value of -1 specifies none
     * \param precision
     *      precision parameter of a printf-specifier, a value of -1 specifies none
     */
    template <typename T>
    static inline void
    printSingleArg(FILE *outputFd,
                   NanoLogInternal::Log::LogMessage &logArguments,
                   const char *formatString,
                   T arg,
                   int width = -1,
                   int precision = -1)
    {
        logArguments.push(arg);

        if (outputFd == nullptr)
            return;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

        if (width < 0 && precision < 0)
        {
            fprintf(outputFd, formatString, arg);
        }
        else if (width >= 0 && precision < 0)
            fprintf(outputFd, formatString, width, arg);
        else if (width >= 0 && precision >= 0)
            fprintf(outputFd, formatString, width, precision, arg);
        else
            fprintf(outputFd, formatString, precision, arg);

#pragma GCC diagnostic pop
    }

    /**
     * Attempt to read back the next log statement contained in the BufferFragment,
     * output the original log message to outputFd, and if applicable, run an
     * aggregation function on the log message.
     *
     * The aggregation function should take in the same arguments as the original
     * invocation of NANO_LOG. For example, NANO_LOG("The number is %d." num) should
     * have the function signature of (const char *fmtString, int num).
     *
     * \param outputFd
     *      File descriptor to output the log messages to
     * \param[in/out] logMsgsProccessed
     *      The number of log messages processed
     * \param lastTimestamp
     *      The timestamp of the last log message to be outputted (this is used
     *      to print time differences).
     * \param checkpoint
     *      The checkpoint containing rdtsc-to-time mapping this function should use
     * \param aggregationFilterId
     *      The logId to target running aggregationFn on
     * \param aggregationFn
     *      This is an aggregation function that can be passed to any log messages
     *      matching aggregationFilterId. This function accepts the same parameters
     *      as the original log statement.
     *
     * \return
     *      true indicates the operation sucessfully; false indicates that either
     *      we reached the end of the file or the file is corrupt.
     */
    bool
    Log::Decoder::BufferFragment::decompressNextLogStatement(FILE *outputFd,
                                                             uint64_t &logMsgsProcessed,
                                                             LogMessage &logArgs,
                                                             const Checkpoint &checkpoint,
                                                             std::vector<void *> &fmtId2metadata,
                                                             long aggregationFilterId,
                                                             void (*aggregationFn)(const char *, ...))
    {
        double secondsSinceCheckpoint, nanos = 0.0;
        char timeString[32];

        if (readPos > endOfBuffer || !hasMoreLogs)
        {
            hasMoreLogs = false;
            return false;
        }

        // no need to format the time if we're not going to output
        if (outputFd)
        {
            // Convert to relative time
            //        double timeDiff;
            //        if (nextLogTimestamp >= lastTimestamp)
            //            timeDiff = 1.0e9*PerfUtils::Cycles::toSeconds(
            //                                    nextLogTimestamp - lastTimestamp,
            //                                    checkpoint.cyclesPerSecond));
            //        else
            //            timeDiff = -1.0e9*PerfUtils::Cycles::toSeconds(
            //                                    lastTimestamp - nextLogTimestamp,
            //                                    checkpoint.cyclesPerSecond));
            //        if (logMsgsProcessed == 0)
            //            timeDiff = 0;
            //
            //        fprintf(outputFd, "%4ld) +%12.2lf ns ", logMsgsProcessed, timeDiff);

            // Convert to absolute time
            secondsSinceCheckpoint = PerfUtils::Cycles::toSeconds(
                nextLogTimestamp - checkpoint.rdtsc,
                checkpoint.cyclesPerSecond);
            int64_t wholeSeconds = static_cast<int64_t>(secondsSinceCheckpoint);
            nanos = 1.0e9 * (secondsSinceCheckpoint - static_cast<double>(wholeSeconds));

            // If the timestamp occurred before the checkpoint, we may have to
            // adjust the times so that nanos remains positive.
            if (nanos < 0.0)
            {
                wholeSeconds--;
                nanos += 1.0e9;
            }

            std::time_t absTime = wholeSeconds + checkpoint.unixTime;
            std::tm *tm = localtime(&absTime);
            strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", tm);
        }

#ifdef PREPROCESSOR_NANOLOG
        if (fmtId2metadata.empty() || aggregationFn != nullptr)
        {
            // Output the context
            struct GeneratedFunctions::LogMetadata meta =
                GeneratedFunctions::logId2Metadata[nextLogId];
            if (outputFd)
            {
                fprintf(outputFd, "%s.%09.0lf %s:%u %s[%u]: ", timeString, nanos, meta.fileName, meta.lineNumber, logLevelNames[meta.logLevel], runtimeId);
            }

            void (*aggFn)(const char *, ...) = nullptr;
            if (aggregationFilterId == nextLogId)
                aggFn = aggregationFn;

            if (nextLogId >= GeneratedFunctions::numLogIds)
            {
                fprintf(stderr, "Log message id=%u not found in the generated "
                                "functions list for Preprocessor NanoLog.\r\n"
                                "This indicates either a corrupt log file or "
                                "a mismatched decompressor as we only have %lu "
                                "generated functions\r\n",
                        nextLogId, GeneratedFunctions::numLogIds);
                return false;
            }

            GeneratedFunctions::decompressAndPrintFnArray[nextLogId](&readPos,
                                                                     outputFd,
                                                                     aggFn);
        }
        else
#endif // PREPROCESSOR_NANOLOG
        {
            using namespace BufferUtils;
            auto *metadata = reinterpret_cast<FormatMetadata *>(
                fmtId2metadata.at(nextLogId));

            const char *filename = metadata->filename;
            const char *logLevel = logLevelNames[metadata->logLevel];

            logArgs.reset(metadata, nextLogId, nextLogTimestamp);

            // Output the context
            if (outputFd)
            {
                fprintf(outputFd, "%s.%09.0lf %s:%u %s[%u]: ", timeString, nanos, filename, metadata->lineNumber, logLevel, runtimeId);
            }

            // Print out the actual log message, piece by piece
            PrintFragment *pf = reinterpret_cast<PrintFragment *>(
                reinterpret_cast<char *>(metadata) + sizeof(FormatMetadata) + metadata->filenameLength);

            Nibbler nb(readPos, metadata->numNibbles);
            const char *nextStringArg = nb.getEndOfPackedArguments();

            // TODO(syang0) We can probably skip processing the log message at
            // if we (a) aren't printing and (b) aren't aggregating
            for (int i = 0; i < metadata->numPrintFragments; ++i)
            {
                const wchar_t *wstrArg;

                int width = -1;
                if (pf->hasDynamicWidth)
                    width = nb.getNext<int>();

                int precision = -1;
                if (pf->hasDynamicPrecision)
                    precision = nb.getNext<int>();

                switch (pf->argType)
                {
                case NONE:

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                    if (outputFd)
                        fprintf(outputFd, pf->formatFragment);
#pragma GCC diagnostic pop
                    break;

                case unsigned_char_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<unsigned char>(),
                                   width, precision);
                    break;

                case unsigned_short_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<unsigned short int>(),
                                   width, precision);
                    break;

                case unsigned_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<unsigned int>(),
                                   width, precision);
                    break;

                case unsigned_long_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<unsigned long int>(),
                                   width, precision);
                    break;

                case unsigned_long_long_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<unsigned long long int>(),
                                   width, precision);
                    break;

                case uintmax_t_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<uintmax_t>(),
                                   width, precision);
                    break;

                case size_t_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<size_t>(),
                                   width, precision);
                    break;

                case wint_t_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<wint_t>(),
                                   width, precision);
                    break;

                case signed_char_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<signed char>(),
                                   width, precision);
                    break;

                case short_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<short int>(),
                                   width, precision);
                    break;

                case int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<int>(),
                                   width, precision);
                    break;

                case long_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<long int>(),
                                   width, precision);
                    break;

                case long_long_int_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<long long int>(),
                                   width, precision);
                    break;

                case intmax_t_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<intmax_t>(),
                                   width, precision);
                    break;

                case ptrdiff_t_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<ptrdiff_t>(),
                                   width, precision);
                    break;

                case double_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<double>(),
                                   width, precision);
                    break;

                case long_double_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<long double>(),
                                   width, precision);
                    break;

                case const_void_ptr_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nb.getNext<const void *>(),
                                   width, precision);
                    break;

                // The next two are strings, so handle it accordingly.
                case const_char_ptr_t:
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   nextStringArg,
                                   width, precision);

                    nextStringArg += strlen(nextStringArg) + 1; // +1 for NULL
                    break;

                case const_wchar_t_ptr_t:

                    /**
                     * I've occasionally encountered the following assertion:
                     * __wcsrtombs: Assertion `data.__outbuf[-1] == '\0'' failed
                     *
                     * I don't know why this occurs, but it appears to be caused
                     * by a wcslen() call deep inside printf returning the wrong
                     * value when called on the dynamic buffer. I've found
                     * that copying the data into a stack buffer first fixes
                     * the problem (not implemented here).
                     *
                     * I don't know why this is the case and I'm inclined to
                     * believe it's a problem with the library because I've...
                     *  (1) verified byte by byte that the copied wchar_t
                     *      strings and the surrounding bytes are exactly the
                     *      same in the dynamic and stack allocated buffers.
                     *  (2) copied the wcslen code from the glib sources into
                     *      to this file and calling it on the dynamic buffer
                     *      works, but the public wcslen() API still returns an
                     *      incorrect value.
                     *  (3) verified that no corruption occurs in the buffers
                     *      before and after the wcslen() returns the wrong val
                     *
                     * If wide character support becomes important and this
                     * assertion keeps erroring out, I would work around it
                     * by copying the wide string into a stack buffer before
                     * passing it to printf.
                     */
                    wstrArg = reinterpret_cast<const wchar_t *>(nextStringArg);
                    printSingleArg(outputFd,
                                   logArgs,
                                   pf->formatFragment,
                                   wstrArg,
                                   width, precision);
                    // +1 for NULL
                    nextStringArg += (wcslen(wstrArg) + 1) * sizeof(wchar_t);
                    break;

                case MAX_FORMAT_TYPE:
                default:
                    fprintf(outputFd,
                            "Error: Corrupt log header in header file\r\n");
                    exit(-1);
                }

                pf = reinterpret_cast<PrintFragment *>(
                    reinterpret_cast<char *>(pf) + pf->fragmentLength + sizeof(PrintFragment));
            }

            if (outputFd)
                fprintf(outputFd, "\r\n");
            // We're done, advance the pointer to the end of the last string
            readPos = nextStringArg;
        }

        logMsgsProcessed++;

        if (readPos >= endOfBuffer)
            hasMoreLogs = false;
        else
            hasMoreLogs = decompressLogHeader(&readPos, nextLogTimestamp,
                                              nextLogId, nextLogTimestamp);

        return true;
    }

    /**
     * Whether one can invoke decompressNextLogStatement or not
     */
    bool
    Log::Decoder::BufferFragment::hasNext()
    {
        return hasMoreLogs;
    }

    /**
     * Return the timestamp of the next log message that can be outputted. It is
     * the responsibility of the caller to ensure that the BufferFragment contains
     * a next log message (i.e. if decompressNextLogStatement returns true).
     */
    uint64_t
    Log::Decoder::BufferFragment::getNextLogTimestamp() const
    {
        assert(readPos <= endOfBuffer && validBytes > 0);
        return nextLogTimestamp;
    }

    /**
     * Decompress the log file that was open()-ed and print the message out in
     * an arbitrary order (i.e. dependent on runtime implementation and not
     * necessarily in chronological order).
     *
     * \param outputFd
     *      The file descriptor to print the log messages to
     * \param aggregationTargetId
     *      Target logId to run the aggregation function on
     * \param aggregationFn
     *      Aggregation function to run on targetId. The signature of the function
     *      should be the same as the original NANO_LOG invocation that created
     *      the log message. For example,
     *          NANO_LOG("number %d, string %s, float %f", num, str, flo)
     *      should have the signature
     *          aggregation(const char*, int, const char*, float)
     *
     * \return
     *      true indicates the operation succeeded without problems.
     *      false indicates that there was an error and an incomplete log was
     *      outputted to outputFd;
     */
    bool
    Log::Decoder::internalDecompressUnordered(FILE *outputFd,
                                              uint32_t aggregationTargetId,
                                              void (*aggregationFn)(const char *, ...))
    {
        if (filename.empty() || !inputFd)
            return false;

        LogMessage logArguments;
        BufferFragment *bf = allocateBufferFragment();
        while (!feof(inputFd) && good)
        {
            bool wrapAround = false;

            EntryType entry = peekEntryType(inputFd);
            switch (entry)
            {
            case EntryType::BUFFER_EXTENT:
            {
                if (!bf->readBufferExtent(inputFd, &wrapAround))
                {
                    fprintf(stderr,
                            "Internal Error: Corrupted BufferExtent\r\n");
                    break;
                }

                ++numBufferFragmentsRead;
                while (bf->hasNext())
                {
                    bf->decompressNextLogStatement(outputFd,
                                                   logMsgsPrinted,
                                                   logArguments,
                                                   checkpoint,
                                                   fmtId2metadata,
                                                   aggregationTargetId,
                                                   aggregationFn);
                }
                break;
            }
            case EntryType::CHECKPOINT:
                if (!readDictionary(inputFd, true))
                    good = false;
                else if (outputFd)
                    fprintf(outputFd, "\r\n# New execution started\r\n");

                break;
            case EntryType::LOG_MSGS_OR_DIC:
                good = readDictionaryFragment(inputFd);
                break;
            case EntryType::INVALID:
                // Consume whitespace
                while (!feof(inputFd) && peekEntryType(inputFd) == INVALID)
                    fgetc(inputFd);
                break;
            }
        }

        if (outputFd)
            fprintf(outputFd, "\r\n\r\n# Decompression Complete after printing "
                              "%lu log messages\r\n",
                    logMsgsPrinted);

        freeBufferFragment(bf);
        return good;
    }

    /**
     * Compares two BufferFragments (a, b) based on the timestamps of
     * their next decompress-able log statements. Returns true if
     * a's timestamp chronologically occurs after b's timestamp.
     *
     * \param a
     *      First BufferFragment to compare
     * \param b
     *      Second BufferFragment to compare
     *
     * \return
     *      True if a > b; False otherwise
     */
    bool
    Log::Decoder::compareBufferFragments(const BufferFragment *a,
                                         const BufferFragment *b)
    {
        return a->getNextLogTimestamp() > b->getNextLogTimestamp();
    };

    /**
     * Decompress the log file that was open()-ed and print the log messages out
     * in chronological order.
     *
     * \param outputFd
     *      The file descriptor to print the log messages to
     *
     * \return
     *      The number of log messages encountered. A negative value indicates error
     */
    int64_t
    Log::Decoder::decompressTo(FILE *outputFd)
    {
        if (filename.empty() || !inputFd)
            return -1;

        // In ordered decompression, we must sort the entries by time which means
        // we need to buffer in 3 rounds of NanoLog output. We need more than one
        // round of output because the compression is non-quiescent, which means
        // that as we're outputting the nth buffer, new entries may be added to
        // n-1 and n, and the entries in n could logically come /before/ the new
        // entries added to n-1. The reason why we need at least 3 is due to an
        // implementation detail in StagingBuffer whereby one peek() does not
        // return all the data and at least 2 peek()'s are needed to deplete a
        // buffer.
        static const uint32_t stagesToBuffer = 3;
        std::vector<BufferFragment *> stages[stagesToBuffer];

        // Running number of stages being kept in stages
        uint32_t stagesBuffered = 0;

        // Indicates that all stages must be depleted before continuing
        // processing the log file. This should only be true when we detect
        // the start of a new execution(s) log appended to inputFd or we
        // reached the end of the current file
        bool mustDepleteAllStages = false;

        LogMessage logArguments;
        while (!feof(inputFd) && good)
        {

            // Step 1: Read in up to a certain number of "stages" of BufferFragments
            mustDepleteAllStages = false;
            while (!feof(inputFd) && good && !mustDepleteAllStages)
            {
                EntryType entry = peekEntryType(inputFd);
                bool newStage = false;

                switch (entry)
                {
                case EntryType::BUFFER_EXTENT:
                {
                    BufferFragment *bf = allocateBufferFragment();
                    good = bf->readBufferExtent(inputFd, &newStage);
                    ++numBufferFragmentsRead;

                    if (good)
                        stages[stagesBuffered].push_back(bf);

                    break;
                }
                case EntryType::CHECKPOINT:
                    // New logical start to the logs detected, at this point
                    // we should make sure we've printed all the buffered logs
                    // before continuing to parse the next logical start.
                    if (!stages[0].empty())
                    {
                        mustDepleteAllStages = true;
                        break;
                    }

                    // We're safe, all the stages are empty
                    good = readDictionary(inputFd, true);

                    if (good)
                        fprintf(outputFd, "\r\n# New execution started\r\n");

                    break;

                case EntryType::LOG_MSGS_OR_DIC:
                    good = readDictionaryFragment(inputFd);
                    break;

                case EntryType::INVALID:
                    // Consume padding
                    while (!feof(inputFd) && peekEntryType(inputFd) == INVALID)
                        fgetc(inputFd);
                    break;
                }

                if (feof(inputFd))
                    mustDepleteAllStages = true;

                // If we reach a logical end to the current stage,
                // make the current stage available for consumption
                bool needFlush = (mustDepleteAllStages || !good);
                if (newStage || (needFlush && !stages[stagesBuffered].empty()))
                    ++stagesBuffered;

                if (stagesBuffered == stagesToBuffer)
                    break;
            }

            // Step 2: Heapify all BufferFragments within the stages from
            // front=max to back=min
            for (auto &stage : stages)
            {
                std::make_heap(stage.begin(), stage.end(), compareBufferFragments);
            }

            // Step 3: Deplete the first stage
            while (true)
            {
                // Step 3a: Find the minimum amongst the stages
                std::vector<BufferFragment *> *minStage = nullptr;
                for (uint32_t i = 0; i < stagesBuffered; ++i)
                {
                    if (stages[i].empty())
                        continue;

                    uint64_t next = stages[i].front()->getNextLogTimestamp();
                    if (minStage == nullptr ||
                        next < minStage->front()->getNextLogTimestamp())
                    {
                        minStage = &(stages[i]);
                    }
                }

                // If nothing was found, we're done
                if (minStage == nullptr)
                {
                    stagesBuffered = 0; // All the stages we know about are clear
                    break;
                }

                // Step 3b: Output the log message
                BufferFragment *bf = minStage->front();
                bf->decompressNextLogStatement(outputFd, logMsgsPrinted,
                                               logArguments, checkpoint,
                                               fmtId2metadata);

                // Moves the minimum element to the end of the array
                std::pop_heap(minStage->begin(), minStage->end(),
                              compareBufferFragments);

                if (bf->hasNext())
                {
                    // If there's more, re-heapify the last element
                    std::push_heap(minStage->begin(), minStage->end(),
                                   compareBufferFragments);
                }
                else
                {
                    // Otherwise, buffer is depleted -> remove it
                    minStage->pop_back();
                    freeBufferFragment(bf);
                }

                // Step 3c: Check for exit condition
                if (stages[0].empty())
                {
                    for (uint32_t i = 0; i < stagesBuffered - 1; ++i)
                    {
                        stages[i] = stages[i + 1];
                    }
                    stages[stagesBuffered - 1].clear();

                    --stagesBuffered;
                    if (!mustDepleteAllStages)
                        break;
                }
            }
        }

        return logMsgsPrinted;
    }

    /**
     * Iterative interface to decompress the next log statement (if there are any)
     * in the log file and optionally prints it via outputFd. The log statements
     * are output in the order in which they appear in the log which may not be
     * in chronological order.
     *
     * \param[out] logMsg
     *          Log message that's decompressed; the contents are valid until
     *          the next invocation to this function.
     *
     * \param outputFd
     *          File descriptor to output the log message to. A value of nullptr
     *          indicates that no printing is desired.
     *
     * \return
     *      True if the decompression was successfully
     *      False indicates there are no more logs or there's an error
     */
    bool
    Log::Decoder::getNextLogStatement(LogMessage &logMsg,
                                      FILE *outputFd)
    {
        if (bufferFragment->hasNext())
        {
            bufferFragment->decompressNextLogStatement(outputFd,
                                                       logMsgsPrinted,
                                                       logMsg,
                                                       checkpoint,
                                                       fmtId2metadata,
                                                       -1,
                                                       nullptr);
            return true;
        }

        logMsg.reset();

        // Decoder was never 'opened' properly
        if (filename.empty() || !inputFd)
            return false;

        // We've read the end of the file or an error
        if (feof(inputFd) || !good)
            return false;

        while (!bufferFragment->hasNext() && !feof(inputFd) && good)
        {
            EntryType entry = peekEntryType(inputFd);
            bool wrapAround;

            switch (entry)
            {
            case EntryType::BUFFER_EXTENT:
                if (bufferFragment->readBufferExtent(inputFd, &wrapAround))
                {
                    ++numBufferFragmentsRead;
                    break;
                }

                fprintf(stderr, "Internal Error: Corrupted BufferExtent\r\n");
                good = false;
                return false;

            case EntryType::CHECKPOINT:
                if (readDictionary(inputFd, true))
                {

                    if (outputFd)
                        fprintf(outputFd, "\r\n# New execution started\r\n");

                    break;
                }

                good = false;
                return false;

            case EntryType::LOG_MSGS_OR_DIC:
                good = readDictionaryFragment(inputFd);
                break;

            case EntryType::INVALID:
                // Consume padding
                while (!feof(inputFd) && peekEntryType(inputFd) == INVALID)
                    fgetc(inputFd);
                break;
            }
        }

        return bufferFragment->decompressNextLogStatement(outputFd,
                                                          logMsgsPrinted,
                                                          logMsg,
                                                          checkpoint,
                                                          fmtId2metadata,
                                                          -1,
                                                          nullptr);
    }

    /**
     * Decompress the file open()-ed to a file descriptor. This invocation will
     * not attempt to sort the log entries by time, but otherwise functions
     * in the same way as decompressTo().
     *
     * \param outputFd
     *      File descriptor to output the log messages to
     * \param logMsgsToPrint
     *      Number of log messages to print before returning
     * \return
     *      The number of log messages processed; a negative value indicates error
     */
    int64_t
    Log::Decoder::decompressUnordered(FILE *outputFd)
    {
        bool success = internalDecompressUnordered(outputFd);
        return (success) ? logMsgsPrinted : -1;
    }

}; /* NanoLogInternal */
