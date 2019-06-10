// This file is part of pydlt
// Copyright 2019  Vladimir Shapranov
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <array>
#include <stdint.h>
#include <type_traits>
#include <string>
#include "bitmasks.h"
#include "parsers.h"
#include "headers.h"
#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <vector>
#include "dltreader.h"
#include "log.h"

const std::array<char, 4> storage_magic{'D', 'L', 'T', '\x01'};

namespace{
    constexpr int MSG_MAX_LEN{2000};
}

DltReader::DltReader(bool expectStorage)
    : iClearStage{expectStorage?Stage::Storage:Stage::Basic}
{
}

off_t DltReader::dataLeft(){
    return iDataEnd - iCursor;
}

void DltReader::consumeMessage(){
    if (iStage != Stage::Done){
        throw std::runtime_error("Can't consume message, it was not fully received yet");
    }
    iStage = iClearStage;
    iMessageBegin = iCursor;
        
}

void DltReader::flush(){
    auto shift = iMessageBegin - iBuffer.begin();
    iDataEnd = std::copy(iMessageBegin, iDataEnd, iBuffer.begin());
    iCursor -= shift;
    iMessageBegin -= shift;
}

bool DltReader::read(){

    if (iStage == Stage::Storage){
        bool ok = fill_struct_if_possible(iCursor, iDataEnd, false,
                                          iStorageHeader.magic,
                                          iStorageHeader.ts_sec,
                                          iStorageHeader.ts_msec,
                                          iStorageHeader.ecu_id
            );        
        if (!ok){
            return false;
        }

        auto& m = iStorageHeader.magic;
        if (!std::equal(
                m.begin(), m.end(),
                storage_magic.begin(), storage_magic.end())){
            fprintf(stderr, "Magic mismatch!");
            throw dlt_corrupted("Magic mismatch!");
        }
        //LOG("Read storage header, ts={}", iStorageHeader.ts_sec);
        iStage = Stage::Basic;
    }

    if (iStage == Stage::Basic){
        // FIXME better "minimal size" value
        if (dataLeft() < 4){
            return false;
        }
        iBasicOffset = iCursor - iMessageBegin;
        auto crs = iCursor;
        crs = read_bitmask(crs, 0,
                           iBasicHeader.version,
                           iBasicHeader.has_tmsp,
                           iBasicHeader.has_seid,
                           iBasicHeader.has_ecu_id,
                           iBasicHeader.big_endian,
                           iBasicHeader.use_ext
            );

        bool bigend = iBasicHeader.big_endian;
        optional_field ecu{
            iBasicHeader.has_ecu_id, iBasicHeader.ecu_id};
        optional_field seid{
            iBasicHeader.has_seid, iBasicHeader.seid};
        optional_field tmsp{
            iBasicHeader.has_tmsp, iBasicHeader.tmsp};
        bool ok = fill_struct_if_possible(
            crs,
            iDataEnd,
            bigend,
            iBasicHeader.mcnt,
            iBasicHeader.msg_len,
            ecu,
            seid,
            tmsp
            );

        if (!ok){
            return false;
        }
            
        // DLT_USER_BUF_MAX_SIZE + some extra bytes
        if (iBasicHeader.msg_len > MSG_MAX_LEN){
            throw dlt_corrupted(fmt::format("Suspicious message length: {}", iBasicHeader.msg_len));
        }

        if (iBasicHeader.use_ext){
            iStage = Stage::Extended;
        } else {
            iStage = Stage::Payload;
        }
        iCursor = crs;
        iPayloadEndOffset = iBasicOffset + iBasicHeader.msg_len;
        //LOG("Read basic header, len={}, ext={}", iBasicHeader.msg_len, (bool)iBasicHeader.use_ext);
        // Here we already know where the end of payload is,
        // therefore let's just wait until we get the full message
        if (iDataEnd < iMessageBegin + iPayloadEndOffset)
            return false;
    }

    if (iStage == Stage::Extended){
        // Here we should already have the whole message in the buffer.
        // But let's be a bit paranoid, just in case
        auto cur = iCursor;
        cur = read_bitmask(
            cur, 0,
            iExtendedHeader.mtin,
            iExtendedHeader.mtsp,
            iExtendedHeader.verbose
            );
        bool ok = fill_struct_if_possible(
            cur, iDataEnd, false,
            iExtendedHeader.arg_count,
            iExtendedHeader.app,
            iExtendedHeader.ctx
            );
        if (!ok){
            throw std::runtime_error("Not enough data for extended header. This should never happen.");
        }
        //LOG("Extended header from {} to {}", iCursor - iMessageBegin, cur - iMessageBegin);
        iCursor = cur;
        iStage = Stage::Payload;
    }

    if (iStage == Stage::Payload){
        iPayloadOffset = iCursor - iMessageBegin;
        //LOG("PL offset {}", iPayloadOffset);
        if (iDataEnd < iMessageBegin + iPayloadEndOffset){
            throw std::runtime_error("Not enough data for payload. This should never happen.");
        }
        iCursor = iMessageBegin + iPayloadEndOffset;
        //LOG("Payload end, next char: {}", (int)*iCursor);
        iStage = Stage::Done;
    }

    return iStage == Stage::Done;
}

void DltReader::resetMessage(){
    iCursor = iMessageBegin;
    iStage = iClearStage;
}
    
bool DltReader::findMagic(){
    if (iCursor == iMessageBegin){
        ++iCursor;
    }
    auto startCur = iCursor;
    while (true){
        if (dataLeft() < 4) return false;
        if (std::equal(
                storage_magic.begin(), storage_magic.end(),
                iCursor, iCursor + 4)){
            iMessageBegin = iCursor;
            LOG("Magic found after {} bytes", iCursor - startCur);
            return true;
        }
        ++iCursor;
    }
}

FilteredDltReader::FilteredDltReader(
    bool expectStorage,
    const std::vector<MsgFilter>& filters,
    bool verboseOnly)
    : DltReader(expectStorage)
    , iFilters(filters)
    , iVerboseOnly(verboseOnly)
    , iAutoRecover(expectStorage)
{} 
                                        

bool FilteredDltReader::checkFilters(){
    auto& basic = getBasic();
    if (!basic.use_ext) return false;
    auto& ext = getExtended();
    if (iVerboseOnly && (!ext.verbose)) return false;
    if (iFilters.empty()) return true;
        
    for (const auto& flt: iFilters){
        if (flt(*this)) return true;
    }
    return false;
}
    
bool FilteredDltReader::readFiltered(){
    while (1){      
        try{
            while (read()){
                if (checkFilters()) return true;
                consumeMessage();
            }
            return false;
        } catch (const dlt_corrupted& ex){
            if (iAutoRecover){
                    
                LOG("File is corrupted: {}, will try to recover", ex.what());
                if (!findMagic()){
                    LOG("End of buffer while looking for magic", "");
                    return false;
                }
            } else {
                LOG("File is corrupted, auto-recover is disabled/not possible", "");
                throw;
            }
        }
    }
}

bool match(const std::array<char, 4>& id, const std::optional<std::array<char, 4>>& idFlt){
    if (!idFlt) return true;
    for (int i = 0; i < 4; i++){
        if (id[i] != (*idFlt)[i]) return false;
        if (id[i] == 0) break;
    }
    return true; 
}

bool MsgFilter::operator()(const DltReader& rdr) const{
    auto& hdr = rdr.getBasic();
    if (!hdr.use_ext) return false;
    auto& ext = rdr.getExtended();
        
    return match(ext.app, app) && match(ext.ctx, ctx);
}