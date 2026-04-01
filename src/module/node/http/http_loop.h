#ifndef Z8_HTTP_LOOP_H
#define Z8_HTTP_LOOP_H

// Forward declarations
struct us_loop_t;

namespace z8 {
namespace module {

class HTTPLoopManager {
  public:
    static HTTPLoopManager& getInstance() {
        static HTTPLoopManager instance;
        return instance;
    }

    us_loop_t* getLoop();
    void tick(); // Poll the loop once
    bool hasActiveServers() const { return m_active_servers > 0; }
    void incrementServers() { m_active_servers++; }
    void decrementServers() { m_active_servers--; }

  private:
    HTTPLoopManager();
    ~HTTPLoopManager();

    us_loop_t* p_loop;
    int m_active_servers;
};

} // namespace module
} // namespace z8

#endif // Z8_HTTP_LOOP_H
