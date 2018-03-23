/*
 * Copyright 2018 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

// Implementation of fastq_reader.h
#include "deepvariant/util/fastq_reader.h"

#include "deepvariant/util/genomics/fastq.pb.h"
#include "deepvariant/util/utils.h"
#include "deepvariant/util/vendor/zlib_compression_options.h"
#include "deepvariant/util/vendor/zlib_inputstream.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/io/buffered_inputstream.h"
#include "tensorflow/core/lib/io/compression.h"
#include "tensorflow/core/lib/io/random_inputstream.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/file_system.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"

namespace nucleus {

namespace tf = tensorflow;

// 256 KB read buffer.
constexpr int READER_BUFFER_SIZE = 256 * 1024 - 1;

constexpr char HEADER_SYMBOL = '@';
constexpr char SEQUENCE_AND_QUALITY_SEPARATOR_SYMBOL = '+';

// -----------------------------------------------------------------------------
//
// Reader for FASTQ formats containing NGS reads.
//
// -----------------------------------------------------------------------------

namespace {
tf::Status ConvertToPb(const string& header, const string& sequence,
                       const string& pad, const string& quality,
                       nucleus::genomics::v1::FastqRecord* record) {
  CHECK(record != nullptr) << "FASTQ record cannot be null";
  record->Clear();

  // Validate the four lines as appropriate.
  if (!header.length() || header[0] != HEADER_SYMBOL ||
      sequence.length() != quality.length()) {
    return tf::errors::DataLoss("Failed to parse FASTQ record");
  }

  size_t spaceix = header.find(" ");
  if (spaceix == string::npos) {
    record->set_id(header.substr(1));
  } else {
    record->set_id(header.substr(1, spaceix - 1));
    record->set_description(header.substr(spaceix + 1));
  }

  record->set_sequence(sequence);
  record->set_quality(quality);

  return tf::Status::OK();
}
}  // namespace

// Iterable class for traversing all FASTQ records in the file.
class FastqFullFileIterable : public FastqIterable {
 public:
  // Advance to the next record.
  StatusOr<bool> Next(nucleus::genomics::v1::FastqRecord* out) override;

  // Constructor is invoked via FastqReader::Iterate.
  FastqFullFileIterable(const FastqReader* reader);
  ~FastqFullFileIterable() override;
};

StatusOr<std::unique_ptr<FastqReader>> FastqReader::FromFile(
    const string& fastq_path,
    const nucleus::genomics::v1::FastqReaderOptions& options) {
  return std::unique_ptr<FastqReader>(new FastqReader(fastq_path, options));
}

FastqReader::FastqReader(
    const string& fastq_path,
    const nucleus::genomics::v1::FastqReaderOptions& options)
    : options_(options) {
  TF_CHECK_OK(
      tf::Env::Default()->NewRandomAccessFile(fastq_path.c_str(), &src_));

  if (options.compression_type() ==
      nucleus::genomics::v1::FastqReaderOptions::GZIP) {
    file_stream_.reset(new tf::io::RandomAccessInputStream(src_.get()));
    zlib_stream_.reset(new tf::io::ZlibInputStream(
        file_stream_.get(), READER_BUFFER_SIZE, READER_BUFFER_SIZE,
        tf::io::ZlibCompressionOptions::GZIP()));
    buffered_inputstream_.reset(new tf::io::BufferedInputStream(
        zlib_stream_.get(), READER_BUFFER_SIZE));
  } else {
    buffered_inputstream_.reset(
        new tf::io::BufferedInputStream(src_.get(), READER_BUFFER_SIZE));
  }
}

FastqReader::~FastqReader() { TF_CHECK_OK(Close()); }

tf::Status FastqReader::Close() {
  buffered_inputstream_.reset();
  zlib_stream_.reset();
  file_stream_.reset();
  src_.reset();
  return tf::Status::OK();
}

tf::Status FastqReader::Next(string* header, string* sequence, string* pad,
                             string* quality) const {
  header->clear();
  sequence->clear();
  pad->clear();
  quality->clear();

  // Read the four lines, returning early if we are at the end of the stream or
  // the record is truncated.
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(header));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(sequence));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(pad));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(quality));

  return tf::Status::OK();
}

StatusOr<std::shared_ptr<FastqIterable>> FastqReader::Iterate() const {
  if (src_ == nullptr)
    return tf::errors::FailedPrecondition(
        "Cannot Iterate a closed FastqReader.");
  return StatusOr<std::shared_ptr<FastqIterable>>(
      MakeIterable<FastqFullFileIterable>(this));
}

// Iterable class definitions.
StatusOr<bool> FastqFullFileIterable::Next(
    nucleus::genomics::v1::FastqRecord* out) {
  TF_RETURN_IF_ERROR(CheckIsAlive());
  const FastqReader* fastq_reader = static_cast<const FastqReader*>(reader_);
  string header;
  string sequence;
  string pad;
  string quality;
  tf::Status status = fastq_reader->Next(&header, &sequence, &pad, &quality);
  if (tf::errors::IsOutOfRange(status)) {
    return false;
  } else if (!status.ok()) {
    return status;
  }
  TF_RETURN_IF_ERROR(ConvertToPb(header, sequence, pad, quality, out));
  return true;
}

FastqFullFileIterable::~FastqFullFileIterable() {}

FastqFullFileIterable::FastqFullFileIterable(const FastqReader* reader)
    : Iterable(reader) {}

}  // namespace nucleus
