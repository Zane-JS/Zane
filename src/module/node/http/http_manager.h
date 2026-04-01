#ifndef Z8_HTTP_MANAGER_H
#define Z8_HTTP_MANAGER_H

#include <vector>

// Forward declarations
struct us_loop_t;

namespace z8 {
namespace module {

class HTTPServer;

// Global HTTP manager - integrates uSockets with Z8 event loop
class HTTPManager {
  public:
    static HTTPManager& getInstance() {
        static HTTPManager instance;
        return instance;
    }

    us_loop_t* getLoop();
    void registerServer(HTTPServer* server);
    void unregisterServer(HTTPServer* server);
    bool hasActiveServers() const;
    void tick(); // Called from Z8's main event loop

  private:
    HTTPManager();
    ~HTTPManager();

    us_loop_t* p_loop;
    std::vector<HTTPServer*> m_servers;
};

} // namespace module
} // namespace z8

#endif // Z8_HTTP_MANAGER_H
