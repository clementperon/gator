/* Copyright (C) 2010-2021 by Arm Limited. All rights reserved. */

#include "Fifo.h"

#include "Logging.h"

#include <cstdlib>

// bufferSize is the amount of data to be filled
// singleBufferSize is the maximum size that may be filled during a single write
// (bufferSize + singleBufferSize) will be allocated
Fifo::Fifo(int singleBufferSize, int bufferSize, sem_t * readerSem)
    : mSingleBufferSize(singleBufferSize),
      mWrite(0),
      mRead(0),
      mReadCommit(0),
      mRaggedEnd(0),
      mWrapThreshold(bufferSize),
      mWaitForSpaceSem(),
      mReaderSem(readerSem),
      mBuffer(new char[bufferSize + singleBufferSize]),
      mEnd(false)
{
    if (mBuffer == nullptr) {
        LOG_ERROR("failed to allocate %d bytes", bufferSize + singleBufferSize);
        handleException();
    }

    if (sem_init(&mWaitForSpaceSem, 0, 0) != 0) {
        LOG_ERROR("sem_init() failed");
        handleException();
    }
}

Fifo::~Fifo()
{
    free(mBuffer);
    sem_destroy(&mWaitForSpaceSem);
}

int Fifo::numBytesFilled() const
{
    return mWrite - mRead + mRaggedEnd;
}

char * Fifo::start() const
{
    return mBuffer;
}

bool Fifo::isEmpty() const
{
    return mRead == mWrite && mRaggedEnd == 0;
}

bool Fifo::isFull() const
{
    return willFill(0);
}

// Determines if the buffer will fill assuming 'additional' bytes will be added to the buffer
// 'full' means there is less than singleBufferSize bytes available contiguously; it does not mean there are zero bytes available
bool Fifo::willFill(int additional) const
{
    if (mWrite > mRead) {
        if (numBytesFilled() + additional < mWrapThreshold) {
            return false;
        }
    }
    else {
        if (numBytesFilled() + additional < mWrapThreshold - mSingleBufferSize) {
            return false;
        }
    }
    return true;
}

// This function will stall until contiguous singleBufferSize bytes are available
char * Fifo::write(int length)
{
    if (length <= 0) {
        length = 0;
        mEnd = true;
    }

    // update the write pointer
    mWrite += length;

    // handle the wrap-around
    if (mWrite >= mWrapThreshold) {
        mRaggedEnd = mWrite;
        mWrite = 0;
    }

    // send a notification that data is ready
    sem_post(mReaderSem);

    // wait for space
    while (isFull()) {
        sem_wait(&mWaitForSpaceSem);
    }

    return &mBuffer[mWrite];
}

void Fifo::release()
{
    // update the read pointer now that the data has been handled
    mRead = mReadCommit;

    // handle the wrap-around
    if (mRead >= mWrapThreshold) {
        mRaggedEnd = mRead = mReadCommit = 0;
    }

    // send a notification that data is free (space is available)
    sem_post(&mWaitForSpaceSem);
}

// This function will return null if no data is available
char * Fifo::read(int * const length)
{
    // wait for data
    if (isEmpty() && !mEnd) {
        return nullptr;
    }

    // obtain the length
    do {
        mReadCommit = mRaggedEnd != 0 ? mRaggedEnd : mWrite;
        *length = mReadCommit - mRead;
    } while (*length < 0); // plugs race condition without using semaphores

    return &mBuffer[mRead];
}
