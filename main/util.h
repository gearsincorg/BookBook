#pragma once
#include <memory>
#include <string>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Small helpers shared by the library client, memory, brain and OTA code.
namespace util {

// A scoped lock on one recursive mutex per module. Each module names its own empty Tag type (in its anonymous
// namespace), so modules never share, or contend for, a mutex:
//     struct LockTag {};
//     using Lock = util::ModuleLock<LockTag>;
template <typename Tag>
class ModuleLock {
public:
    ModuleLock() { xSemaphoreTakeRecursive(handle(), portMAX_DELAY); }
    ~ModuleLock() { xSemaphoreGiveRecursive(handle()); }
    ModuleLock(const ModuleLock&) = delete;
    ModuleLock& operator=(const ModuleLock&) = delete;

private:
    static SemaphoreHandle_t handle() {
        static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
        return h;
    }
};

struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

// Compact JSON text of `j`, which is deleted afterwards unless `take` is false.
inline std::string print(cJSON* j, bool take = true) {
    char* s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "";
    cJSON_free(s);
    if (take) cJSON_Delete(j);
    return out;
}

// The memory URL points at .../memory.json?<sas>; the same container token reaches the container's other blobs
// (standby.json, bookbook.bin, bookbook.json). Returns the URL of `blob` in the same container.
inline std::string blob_url(const std::string& base, const char* blob) {
    const size_t q = base.find('?');
    const std::string path = base.substr(0, q);
    const std::string query = q == std::string::npos ? "" : base.substr(q);
    const size_t slash = path.rfind('/');
    return path.substr(0, slash + 1) + blob + query;
}

}  // namespace util
