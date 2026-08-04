#include "dage/tools/resource_providers.hpp"
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dage { namespace tools {
namespace {
Error error(const std::string& code,const std::string& message){
    Error value;value.category="resource";value.code=code;value.message=message;return value;
}
bool within(const std::filesystem::path& root,const std::filesystem::path& target){
    auto a=root.begin(),b=target.begin();
    for(;a!=root.end();++a,++b)if(b==target.end()||*a!=*b)return false;
    return true;
}
bool link_like(const std::filesystem::path& path){
    if(std::filesystem::is_symlink(std::filesystem::symlink_status(path)))return true;
#ifdef _WIN32
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
#else
    return false;
#endif
}
bool contains_symlink(const std::filesystem::path& root,const std::filesystem::path& relative){
    std::filesystem::path current=root;
    for(const auto& component:relative){current/=component;
        if(link_like(current))return true;}
    return false;
}
bool valid_utf8(const std::string& text){
    for(std::size_t i=0;i<text.size();){
        const unsigned char c=static_cast<unsigned char>(text[i]);std::size_t count=0;
        if(c<0x80){++i;continue;}if((c&0xe0)==0xc0){count=2;if(c<0xc2)return false;}
        else if((c&0xf0)==0xe0)count=3;else if((c&0xf8)==0xf0){count=4;if(c>0xf4)return false;}else return false;
        if(i+count>text.size())return false;
        for(std::size_t j=1;j<count;++j)if((static_cast<unsigned char>(text[i+j])&0xc0)!=0x80)return false;
        if(count==3){const unsigned char b=static_cast<unsigned char>(text[i+1]);if((c==0xe0&&b<0xa0)||(c==0xed&&b>=0xa0))return false;}
        if(count==4){const unsigned char b=static_cast<unsigned char>(text[i+1]);if((c==0xf0&&b<0x90)||(c==0xf4&&b>=0x90))return false;}
        i+=count;
    }return true;
}
bool safe_component(const std::string& value){
    if(value.empty()||value=="."||value=="..")return false;
    return std::all_of(value.begin(),value.end(),[](unsigned char c){
        return std::isalnum(c)||c=='.'||c=='_'||c=='-';});
}
std::size_t depth(const std::string& path){return 1+static_cast<std::size_t>(std::count(path.begin(),path.end(),'/'));}
std::uint16_t u16(const ResourceBytes& b,std::size_t p){
    if(p+2>b.size())throw std::runtime_error("ZIP_TRUNCATED");
    return static_cast<std::uint16_t>(b[p]|(static_cast<std::uint16_t>(b[p+1])<<8));
}
std::uint32_t u32(const ResourceBytes& b,std::size_t p){
    if(p+4>b.size())throw std::runtime_error("ZIP_TRUNCATED");
    return static_cast<std::uint32_t>(b[p])|(static_cast<std::uint32_t>(b[p+1])<<8)|
        (static_cast<std::uint32_t>(b[p+2])<<16)|(static_cast<std::uint32_t>(b[p+3])<<24);
}
void validate_limits(const ResourceLimits& limits){
    if(!limits.max_resources||!limits.max_resource_bytes||!limits.max_total_bytes||
       !limits.max_path_bytes||!limits.max_depth)throw std::invalid_argument("resource limits must be positive");
}
}

DirectoryResourceProvider::DirectoryResourceProvider(std::filesystem::path root,ResourceLimits limits)
    :root_(std::filesystem::canonical(std::move(root))),limits_(limits){
    validate_limits(limits_);
    if(!std::filesystem::is_directory(root_))throw std::invalid_argument("resource root is not a directory");
}
Result<std::vector<std::string>> DirectoryResourceProvider::list_resources()const{
    try{
        std::vector<std::string> paths;std::uint64_t total=0;
        std::filesystem::recursive_directory_iterator iterator(root_),end;
        for(;iterator!=end;++iterator){
            const auto status=iterator->symlink_status();
            if(link_like(iterator->path()))
                return Result<std::vector<std::string>>::failure(error("SYMLINK_REJECTED","symbolic links and reparse links are not resources"));
            if(!std::filesystem::is_regular_file(status))continue;
            const std::filesystem::path canonical=std::filesystem::canonical(iterator->path());
            if(!within(root_,canonical))
                return Result<std::vector<std::string>>::failure(error("PATH_ESCAPE","resource escaped directory root"));
            const std::string path=std::filesystem::relative(canonical,root_).generic_string();
            Result<std::string> normalized=normalize_resource_path(path);if(!normalized)
                return Result<std::vector<std::string>>::failure(normalized.error());
            if(path.size()>limits_.max_path_bytes||depth(path)>limits_.max_depth)
                return Result<std::vector<std::string>>::failure(error("RESOURCE_PATH_LIMIT","resource path limit exceeded"));
            const std::uint64_t size=std::filesystem::file_size(canonical);
            if(size>limits_.max_resource_bytes||size>limits_.max_total_bytes||total>limits_.max_total_bytes-size)
                return Result<std::vector<std::string>>::failure(error("RESOURCE_SIZE_LIMIT","resource size limit exceeded"));
            total+=size;paths.push_back(path);
            if(paths.size()>limits_.max_resources)
                return Result<std::vector<std::string>>::failure(error("RESOURCE_COUNT_LIMIT","resource count limit exceeded"));
        }
        std::sort(paths.begin(),paths.end());
        return Result<std::vector<std::string>>::success(std::move(paths));
    }catch(const std::exception& ex){
        return Result<std::vector<std::string>>::failure(error("DIRECTORY_LIST_FAILED",ex.what()));
    }
}
Result<ResourceBytes> DirectoryResourceProvider::read_resource(const std::string& path)const{
    Result<std::string> normalized=normalize_resource_path(path);if(!normalized)return Result<ResourceBytes>::failure(normalized.error());
    if(path.size()>limits_.max_path_bytes||depth(path)>limits_.max_depth)
        return Result<ResourceBytes>::failure(error("RESOURCE_PATH_LIMIT","resource path limit exceeded"));
    try{
        const std::filesystem::path unresolved=root_/std::filesystem::path(path);
        if(contains_symlink(root_,std::filesystem::path(path)))
            return Result<ResourceBytes>::failure(error("SYMLINK_REJECTED","symbolic links and reparse links are not resources"));
        const std::filesystem::path target=std::filesystem::canonical(unresolved);
        if(!within(root_,target)||!std::filesystem::is_regular_file(target))
            return Result<ResourceBytes>::failure(error("RESOURCE_NOT_FOUND","resource is outside root or not a regular file"));
        const std::uint64_t before=std::filesystem::file_size(target);
        if(before>limits_.max_resource_bytes)return Result<ResourceBytes>::failure(error("RESOURCE_SIZE_LIMIT","resource size limit exceeded"));
        std::ifstream stream(target,std::ios::binary);if(!stream)return Result<ResourceBytes>::failure(error("RESOURCE_NOT_FOUND","resource cannot be opened"));
        ResourceBytes bytes((std::istreambuf_iterator<char>(stream)),{});
        if(bytes.size()!=before||std::filesystem::file_size(target)!=before)
            return Result<ResourceBytes>::failure(error("RESOURCE_CHANGED","resource changed while it was read"));
        return Result<ResourceBytes>::success(std::move(bytes));
    }catch(const std::filesystem::filesystem_error& ex){
        return Result<ResourceBytes>::failure(error("RESOURCE_NOT_FOUND",ex.what()));
    }catch(const std::exception& ex){
        return Result<ResourceBytes>::failure(error("DIRECTORY_READ_FAILED",ex.what()));
    }
}

class ZipResourceProvider::Impl {
public:
    struct Entry {std::uint16_t flags=0,method=0;std::uint32_t crc=0,compressed=0,size=0,offset=0;};
    ResourceBytes archive;ZipLimits limits;std::map<std::string,Entry> entries;
};
namespace {
std::shared_ptr<const ZipResourceProvider::Impl> parse_zip(ResourceBytes archive,const ZipLimits& limits){
    validate_limits(limits);if(!limits.max_archive_bytes||!limits.max_compression_ratio)
        throw std::invalid_argument("ZIP limits must be positive");
    if(archive.size()>limits.max_archive_bytes)throw std::runtime_error("ZIP_ARCHIVE_LIMIT");
    if(archive.size()<22)throw std::runtime_error("ZIP_EOCD_NOT_FOUND");
    const std::size_t lower=archive.size()>65557?archive.size()-65557:0;std::size_t eocd=archive.size()-22;
    for(;;){if(u32(archive,eocd)==0x06054b50)break;if(eocd==lower)throw std::runtime_error("ZIP_EOCD_NOT_FOUND");--eocd;}
    if(eocd+22ull+u16(archive,eocd+20)!=archive.size())throw std::runtime_error("ZIP_EOCD_INVALID");
    if(u16(archive,eocd+4)||u16(archive,eocd+6))throw std::runtime_error("ZIP_MULTIDISK_UNSUPPORTED");
    const std::uint16_t count=u16(archive,eocd+10);const std::uint32_t central_size=u32(archive,eocd+12);
    const std::uint32_t central_offset=u32(archive,eocd+16);
    if(count==0xffff||central_size==0xffffffffu||central_offset==0xffffffffu)throw std::runtime_error("ZIP64_UNSUPPORTED");
    if(count>limits.max_resources||static_cast<std::uint64_t>(central_offset)+central_size>eocd)
        throw std::runtime_error("ZIP_CENTRAL_DIRECTORY_INVALID");
    std::shared_ptr<ZipResourceProvider::Impl> impl(new ZipResourceProvider::Impl());
    impl->archive=std::move(archive);impl->limits=limits;std::size_t p=central_offset;std::uint64_t total=0;
    for(std::uint16_t index=0;index<count;++index){
        if(u32(impl->archive,p)!=0x02014b50)throw std::runtime_error("ZIP_CENTRAL_ENTRY_INVALID");
        const std::uint16_t made=u16(impl->archive,p+4),flags=u16(impl->archive,p+8),method=u16(impl->archive,p+10);
        const std::uint32_t crc=u32(impl->archive,p+16),compressed=u32(impl->archive,p+20),size=u32(impl->archive,p+24);
        const std::uint16_t name_len=u16(impl->archive,p+28),extra_len=u16(impl->archive,p+30),comment_len=u16(impl->archive,p+32);
        const std::uint32_t external=u32(impl->archive,p+38),offset=u32(impl->archive,p+42);
        const std::size_t next=p+46ull+name_len+extra_len+comment_len;if(next>impl->archive.size())throw std::runtime_error("ZIP_TRUNCATED");
        std::string name(reinterpret_cast<const char*>(impl->archive.data()+p+46),name_len);p=next;
        if(name.empty()||name.back()=='/')continue;
        if((flags&~static_cast<std::uint16_t>(0x0808))!=0)throw std::runtime_error("ZIP_FLAGS_UNSUPPORTED");
        if(!valid_utf8(name))throw std::runtime_error("ZIP_FILENAME_ENCODING_INVALID");
        if((made>>8)==3&&((external>>16)&0170000)==0120000)throw std::runtime_error("ZIP_SYMLINK_REJECTED");
        Result<std::string> normalized=normalize_resource_path(name);if(!normalized)throw std::runtime_error("ZIP_INVALID_PATH");
        if(name.size()>limits.max_path_bytes||depth(name)>limits.max_depth)throw std::runtime_error("ZIP_PATH_LIMIT");
        if(flags&1)throw std::runtime_error("ZIP_ENCRYPTED_UNSUPPORTED");
        if(method!=0&&method!=8)throw std::runtime_error("ZIP_COMPRESSION_UNSUPPORTED");
        if(method==8&&size&&!compressed)throw std::runtime_error("ZIP_COMPRESSED_SIZE_INVALID");
        if(size>limits.max_resource_bytes||size>limits.max_total_bytes||total>limits.max_total_bytes-size)throw std::runtime_error("ZIP_SIZE_LIMIT");
        if(size&&compressed&&static_cast<std::uint64_t>(size)>static_cast<std::uint64_t>(compressed)*limits.max_compression_ratio)
            throw std::runtime_error("ZIP_COMPRESSION_RATIO_LIMIT");
        if(impl->entries.count(name))throw std::runtime_error("ZIP_DUPLICATE_ENTRY");
        total+=size;impl->entries[name]=ZipResourceProvider::Impl::Entry{flags,method,crc,compressed,size,offset};
    }
    if(p!=static_cast<std::size_t>(central_offset)+central_size)throw std::runtime_error("ZIP_CENTRAL_SIZE_MISMATCH");
    return impl;
}
}
ZipResourceProvider::ZipResourceProvider(const std::filesystem::path& path,ZipLimits limits){
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("ZIP_NOT_FOUND");
    ResourceBytes bytes((std::istreambuf_iterator<char>(stream)),{});impl_=parse_zip(std::move(bytes),limits);
}
ZipResourceProvider::ZipResourceProvider(ResourceBytes archive,ZipLimits limits):impl_(parse_zip(std::move(archive),limits)){}
ZipResourceProvider::~ZipResourceProvider()=default;
Result<std::vector<std::string>> ZipResourceProvider::list_resources()const{
    std::vector<std::string> names;for(const auto& item:impl_->entries)names.push_back(item.first);
    return Result<std::vector<std::string>>::success(std::move(names));
}
Result<ResourceBytes> ZipResourceProvider::read_resource(const std::string& path)const{
    Result<std::string> normalized=normalize_resource_path(path);if(!normalized)return Result<ResourceBytes>::failure(normalized.error());
    auto found=impl_->entries.find(path);if(found==impl_->entries.end())
        return Result<ResourceBytes>::failure(error("RESOURCE_NOT_FOUND","ZIP resource not found"));
    try{
        const Impl::Entry& entry=found->second;const std::size_t p=entry.offset;
        if(u32(impl_->archive,p)!=0x04034b50)throw std::runtime_error("ZIP_LOCAL_HEADER_INVALID");
        const std::uint16_t flags=u16(impl_->archive,p+6),method=u16(impl_->archive,p+8);
        const std::uint16_t name_len=u16(impl_->archive,p+26),extra_len=u16(impl_->archive,p+28);
        const std::size_t data=p+30ull+name_len+extra_len;
        if(flags!=entry.flags||method!=entry.method||data+entry.compressed>impl_->archive.size())
            throw std::runtime_error("ZIP_LOCAL_HEADER_MISMATCH");
        std::string local_name(reinterpret_cast<const char*>(impl_->archive.data()+p+30),name_len);
        if(local_name!=path)throw std::runtime_error("ZIP_LOCAL_NAME_MISMATCH");
        ResourceBytes output(entry.size);
        if(entry.method==0){
            if(entry.compressed!=entry.size)throw std::runtime_error("ZIP_STORED_SIZE_MISMATCH");
            std::copy_n(impl_->archive.begin()+static_cast<std::ptrdiff_t>(data),entry.size,output.begin());
        }else{
            z_stream stream{};stream.next_in=const_cast<Bytef*>(impl_->archive.data()+data);
            stream.avail_in=entry.compressed;stream.next_out=output.data();stream.avail_out=entry.size;
            if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)throw std::runtime_error("ZIP_INFLATE_INIT_FAILED");
            const int status=inflate(&stream,Z_FINISH);inflateEnd(&stream);
            if(status!=Z_STREAM_END||stream.total_out!=entry.size||stream.total_in!=entry.compressed)
                throw std::runtime_error("ZIP_INFLATE_FAILED");
        }
        const uLong actual=crc32(0L,output.data(),static_cast<uInt>(output.size()));
        if(actual!=entry.crc)throw std::runtime_error("ZIP_CRC_MISMATCH");
        return Result<ResourceBytes>::success(std::move(output));
    }catch(const std::exception& ex){
        return Result<ResourceBytes>::failure(error(ex.what(),ex.what()));
    }
}

DirectoryBundleRepository::DirectoryBundleRepository(std::filesystem::path root,ResourceLimits limits)
    :root_(std::filesystem::canonical(std::move(root))),limits_(limits){validate_limits(limits_);
    if(!std::filesystem::is_directory(root_))throw std::invalid_argument("repository root is not a directory");}
Result<std::vector<std::string>> DirectoryBundleRepository::available_versions(const std::string& id,bool)const{
    if(!safe_component(id))return Result<std::vector<std::string>>::failure(error("BUNDLE_ID_INVALID","invalid Bundle id"));
    try{std::vector<std::string> versions;const std::filesystem::path directory=root_/id;
        if(link_like(directory)||!std::filesystem::is_directory(directory))
            return Result<std::vector<std::string>>::failure(error("BUNDLE_NOT_FOUND","Bundle not found"));
        for(const auto& entry:std::filesystem::directory_iterator(directory))
            if(entry.is_directory()&&!link_like(entry.path())&&safe_component(entry.path().filename().string()))
                versions.push_back(entry.path().filename().string());
        std::sort(versions.begin(),versions.end());return Result<std::vector<std::string>>::success(std::move(versions));
    }catch(const std::exception& ex){return Result<std::vector<std::string>>::failure(error("REPOSITORY_LIST_FAILED",ex.what()));}
}
Result<std::shared_ptr<const ResourceProvider>> DirectoryBundleRepository::get(
    const std::string& id,const std::string& version,bool)const{
    if(!safe_component(id)||!safe_component(version))
        return Result<std::shared_ptr<const ResourceProvider>>::failure(error("REPOSITORY_COORDINATE_INVALID","invalid Bundle coordinate"));
    try{const std::filesystem::path relative=std::filesystem::path(id)/version;
        if(contains_symlink(root_,relative))throw std::runtime_error("repository coordinate contains a symlink");
        const std::filesystem::path target=std::filesystem::canonical(root_/relative);
        if(!within(root_,target))throw std::runtime_error("repository coordinate escaped root");
        return Result<std::shared_ptr<const ResourceProvider>>::success(
        std::make_shared<DirectoryResourceProvider>(target,limits_));
    }catch(const std::exception& ex){return Result<std::shared_ptr<const ResourceProvider>>::failure(error("VERSION_NOT_FOUND",ex.what()));}
}
std::string DirectoryBundleRepository::source()const{return root_.generic_string();}

ZipBundleRepository::ZipBundleRepository(std::filesystem::path root,ZipLimits limits)
    :root_(std::filesystem::canonical(std::move(root))),limits_(limits){validate_limits(limits_);
    if(!std::filesystem::is_directory(root_))throw std::invalid_argument("repository root is not a directory");}
Result<std::vector<std::string>> ZipBundleRepository::available_versions(const std::string& id,bool)const{
    if(!safe_component(id))return Result<std::vector<std::string>>::failure(error("BUNDLE_ID_INVALID","invalid Bundle id"));
    try{std::vector<std::string> versions;const std::filesystem::path directory=root_/id;
        if(link_like(directory)||!std::filesystem::is_directory(directory))
            return Result<std::vector<std::string>>::failure(error("BUNDLE_NOT_FOUND","Bundle not found"));
        for(const auto& entry:std::filesystem::directory_iterator(directory)){
            if(!entry.is_regular_file()||link_like(entry.path())||entry.path().extension()!=".zip")continue;
            const std::string version=entry.path().stem().string();if(safe_component(version))versions.push_back(version);
        }
        std::sort(versions.begin(),versions.end());return Result<std::vector<std::string>>::success(std::move(versions));
    }catch(const std::exception& ex){return Result<std::vector<std::string>>::failure(error("REPOSITORY_LIST_FAILED",ex.what()));}
}
Result<std::shared_ptr<const ResourceProvider>> ZipBundleRepository::get(
    const std::string& id,const std::string& version,bool)const{
    if(!safe_component(id)||!safe_component(version))
        return Result<std::shared_ptr<const ResourceProvider>>::failure(error("REPOSITORY_COORDINATE_INVALID","invalid Bundle coordinate"));
    try{const std::filesystem::path relative=std::filesystem::path(id)/(version+".zip");
        if(contains_symlink(root_,relative))throw std::runtime_error("repository coordinate contains a symlink");
        const std::filesystem::path target=std::filesystem::canonical(root_/relative);
        if(!within(root_,target))throw std::runtime_error("repository coordinate escaped root");
        return Result<std::shared_ptr<const ResourceProvider>>::success(
        std::make_shared<ZipResourceProvider>(target,limits_));
    }catch(const std::exception& ex){return Result<std::shared_ptr<const ResourceProvider>>::failure(error("VERSION_NOT_FOUND",ex.what()));}
}
std::string ZipBundleRepository::source()const{return root_.generic_string();}
}} // namespace dage::tools
