// SPDX-FileCopyrightText: 2017-2025 Carl Zeiss Microscopy GmbH
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "executeReEncodeCzi.h"
#include "executeBase.h"
#include "inc_libCZI.h"
#include "libCZI_compress.h"
#include "utils.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace libCZI;
using namespace std;

namespace
{
    static shared_ptr<IMemoryBlock> EncodeTile(
        CompressionMode target,
        PixelType pixelType,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        const void* pixels,
        const ICompressParameters* compressParams)
    {
        switch (target)
        {
        case CompressionMode::Zstd0:
            return ZstdCompress::CompressZStd0Alloc(width, height, stride, pixelType, pixels, compressParams);
        case CompressionMode::Zstd1:
            return ZstdCompress::CompressZStd1Alloc(width, height, stride, pixelType, pixels, compressParams);
        case CompressionMode::JpgXr:
            return JxrLibCompress::Compress(pixelType, width, height, stride, pixels, compressParams);
#if LIBCZI_HAVE_LIBJXL
        case CompressionMode::Jxl:
            return JxlLibCompress::Compress(pixelType, width, height, stride, pixels, compressParams);
#else
        case CompressionMode::Jxl:
            throw runtime_error("ReEncodeCZI: JPEG XL output requires LIBCZI_BUILD_WITH_LIBJXL=ON.");
#endif
        default:
            break;
        }

        stringstream ss;
        ss << "ReEncodeCZI: unsupported target compression \""
           << Utils::CompressionModeToInformalString(target)
           << "\" (supported: zstd0, zstd1, jpgxr, jxl";
#if !LIBCZI_HAVE_LIBJXL
        ss << " (jxl not built)";
#endif
        ss << ", uncompressed).";
        throw runtime_error(ss.str());
    }
}

class CExecuteReEncodeCzi : CExecuteBase
{
public:
    static bool execute(const CCmdLineOptions& options);
};

bool CExecuteReEncodeCzi::execute(const CCmdLineOptions& options)
{
    const CompressionMode targetCompression = options.GetCompressionMode();
    if (targetCompression == CompressionMode::Invalid)
    {
        throw invalid_argument("ReEncodeCZI requires --compressionopts (e.g. zstd1:ExplicitLevel=3 or jxl:).");
    }

    auto reader = CExecuteReEncodeCzi::CreateAndOpenCziReader(options);
    const FileHeaderInfo hdr = reader->GetFileHeaderInfo();
    SubBlockStatistics stats = reader->GetStatistics();
    if (stats.subBlockCount <= 0)
    {
        throw runtime_error("ReEncodeCZI: source has no subblocks.");
    }

    const wstring outPath = options.MakeOutputFilename(nullptr, L"czi");
    auto outStream = CreateOutputStreamForFile(outPath.c_str(), true);
    auto writer = CreateCZIWriter();

    auto writerInfo = make_shared<CCziWriterInfo>(hdr.fileGuid);
    if (!stats.dimBounds.IsEmpty())
    {
        writerInfo->SetDimBounds(&stats.dimBounds);
    }

    if (stats.IsMIndexValid())
    {
        writerInfo->SetMIndexBounds(stats.minMindex, stats.maxMindex);
    }

    writerInfo->SetReservedSizeForSubBlockDirectory(true, static_cast<size_t>(max(1, stats.subBlockCount)));

    int attachmentCount = 0;
    reader->EnumerateAttachments(
        [&](int, const AttachmentInfo&)->bool
        {
            ++attachmentCount;
            return true;
        });
    writerInfo->SetReservedSizeForAttachmentsDirectory(true, static_cast<size_t>(max(1, attachmentCount)));

    writer->Create(outStream, writerInfo);

    const ICompressParameters* compressParams = options.GetCompressionParameters().get();

    reader->EnumerateSubBlocks(
        [&](int index, const SubBlockInfo&)->bool
        {
            auto sb = reader->ReadSubBlock(index);
            if (sb == nullptr)
            {
                stringstream ss;
                ss << "ReEncodeCZI: ReadSubBlock(" << index << ") failed.";
                throw runtime_error(ss.str());
            }

            const SubBlockInfo& sbi = sb->GetSubBlockInfo();
            shared_ptr<IBitmapData> bitmap = sb->CreateBitmap(nullptr);
            ScopedBitmapLockerSP locker(bitmap);

            vector<uint8_t> metaCopy;
            size_t metaSize = 0;
            const auto metaBlob = sb->GetRawData(ISubBlock::MemBlkType::Metadata, &metaSize);
            if (metaSize > 0 && metaBlob != nullptr)
            {
                const auto* p = static_cast<const uint8_t*>(metaBlob.get());
                metaCopy.assign(p, p + metaSize);
            }

            vector<uint8_t> attCopy;
            size_t attSize = 0;
            const auto attBlob = sb->GetRawData(ISubBlock::MemBlkType::Attachment, &attSize);
            if (attSize > 0 && attBlob != nullptr)
            {
                const auto* p = static_cast<const uint8_t*>(attBlob.get());
                attCopy.assign(p, p + attSize);
            }

            if (targetCompression == CompressionMode::UnCompressed)
            {
                AddSubBlockInfoStridedBitmap addStrided;
                addStrided.Clear();
                addStrided.coordinate = sbi.coordinate;
                addStrided.mIndexValid = sbi.IsMindexValid();
                addStrided.mIndex = sbi.mIndex;
                addStrided.x = sbi.logicalRect.x;
                addStrided.y = sbi.logicalRect.y;
                addStrided.logicalWidth = sbi.logicalRect.w;
                addStrided.logicalHeight = sbi.logicalRect.h;
                addStrided.physicalWidth = sbi.physicalSize.w;
                addStrided.physicalHeight = sbi.physicalSize.h;
                addStrided.PixelType = sbi.pixelType;
                addStrided.pyramid_type = sbi.pyramidType;
                addStrided.ptrBitmap = locker.ptrDataRoi;
                addStrided.strideBitmap = locker.stride;
                addStrided.ptrSbBlkMetadata = metaCopy.empty() ? nullptr : metaCopy.data();
                addStrided.sbBlkMetadataSize = static_cast<uint32_t>(metaCopy.size());
                addStrided.ptrSbBlkAttachment = attCopy.empty() ? nullptr : attCopy.data();
                addStrided.sbBlkAttachmentSize = static_cast<uint32_t>(attCopy.size());
                writer->SyncAddSubBlock(addStrided);
            }
            else
            {
                shared_ptr<IMemoryBlock> compressed = EncodeTile(
                    targetCompression,
                    sbi.pixelType,
                    bitmap->GetWidth(),
                    bitmap->GetHeight(),
                    locker.stride,
                    locker.ptrDataRoi,
                    compressParams);

                AddSubBlockInfoMemPtr addMem;
                addMem.Clear();
                addMem.coordinate = sbi.coordinate;
                addMem.mIndexValid = sbi.IsMindexValid();
                addMem.mIndex = sbi.mIndex;
                addMem.x = sbi.logicalRect.x;
                addMem.y = sbi.logicalRect.y;
                addMem.logicalWidth = sbi.logicalRect.w;
                addMem.logicalHeight = sbi.logicalRect.h;
                addMem.physicalWidth = sbi.physicalSize.w;
                addMem.physicalHeight = sbi.physicalSize.h;
                addMem.PixelType = sbi.pixelType;
                addMem.pyramid_type = sbi.pyramidType;
                addMem.SetCompressionMode(targetCompression);
                addMem.ptrData = compressed->GetPtr();
                addMem.dataSize = static_cast<uint32_t>(compressed->GetSizeOfData());
                addMem.ptrSbBlkMetadata = metaCopy.empty() ? nullptr : metaCopy.data();
                addMem.sbBlkMetadataSize = static_cast<uint32_t>(metaCopy.size());
                addMem.ptrSbBlkAttachment = attCopy.empty() ? nullptr : attCopy.data();
                addMem.sbBlkAttachmentSize = static_cast<uint32_t>(attCopy.size());
                writer->SyncAddSubBlock(addMem);
            }

            return true;
        });

    reader->EnumerateAttachments(
        [&](int index, const AttachmentInfo&)->bool
        {
            auto att = reader->ReadAttachment(index);
            if (att == nullptr)
            {
                stringstream ss;
                ss << "ReEncodeCZI: ReadAttachment(" << index << ") failed.";
                throw runtime_error(ss.str());
            }

            size_t dataSize = 0;
            const auto blob = att->GetRawData(&dataSize);
            vector<uint8_t> dataCopy;
            if (dataSize > 0 && blob != nullptr)
            {
                const auto* p = static_cast<const uint8_t*>(blob.get());
                dataCopy.assign(p, p + dataSize);
            }

            const AttachmentInfo& ai = att->GetAttachmentInfo();
            AddAttachmentInfo addAtt;
            addAtt.Clear();
            addAtt.contentGuid = ai.contentGuid;
            addAtt.SetContentFileType(ai.contentFileType);
            addAtt.SetName(ai.name.c_str());
            addAtt.ptrData = dataCopy.empty() ? nullptr : dataCopy.data();
            addAtt.dataSize = static_cast<uint32_t>(dataCopy.size());
            writer->SyncAddAttachment(addAtt);
            return true;
        });

    shared_ptr<IMetadataSegment> mdSeg = reader->ReadMetadataSegment();
    if (mdSeg == nullptr)
    {
        throw runtime_error("ReEncodeCZI: source has no metadata segment.");
    }

    size_t xmlSize = 0;
    const shared_ptr<const void> xmlBlob = mdSeg->GetRawData(IMetadataSegment::XmlMetadata, &xmlSize);
    size_t mdAttSize = 0;
    const shared_ptr<const void> mdAttBlob = mdSeg->GetRawData(IMetadataSegment::Attachment, &mdAttSize);

    string mdXmlStr;
    if (xmlSize > 0 && xmlBlob != nullptr)
    {
        mdXmlStr.assign(static_cast<const char*>(xmlBlob.get()), xmlSize);
    }

    vector<uint8_t> mdSegAttStorage;
    if (mdAttSize > 0 && mdAttBlob != nullptr)
    {
        const auto* p = static_cast<const uint8_t*>(mdAttBlob.get());
        mdSegAttStorage.assign(p, p + mdAttSize);
    }

    reader->Close();
    reader.reset();

    WriteMetadataInfo wmi;
    wmi.Clear();
    wmi.szMetadata = mdXmlStr.empty() ? "" : mdXmlStr.c_str();
    wmi.szMetadataSize = mdXmlStr.size();
    wmi.ptrAttachment = mdSegAttStorage.empty() ? nullptr : mdSegAttStorage.data();
    wmi.attachmentSize = mdSegAttStorage.size();
    writer->SyncWriteMetadata(wmi);

    writer->Close();

    wstringstream msg;
    msg << L"ReEncodeCZI wrote \"" << outPath << L"\" (" << stats.subBlockCount << L" subblocks, "
        << attachmentCount << L" attachments).";
    options.GetLog()->WriteLineStdOut(msg.str().c_str());

    return true;
}

bool executeReEncodeCzi(const CCmdLineOptions& options)
{
    return CExecuteReEncodeCzi::execute(options);
}
