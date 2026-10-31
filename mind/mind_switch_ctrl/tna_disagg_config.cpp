#include "tna_disagg_bfrt.hpp"
#include "controller/config.h"

namespace bfrt
{
  namespace tna_disagg_switch
  {
    const int ports_comp[BFRT_NUM_COMPUTE_NODE] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0};
    const char *ips_comp[BFRT_NUM_COMPUTE_NODE] = {"0.0.0.0", "0.0.0.0", "0.0.0.0", "0.0.0.0",
                                                   "0.0.0.0", "0.0.0.0", "0.0.0.0", "0.0.0.0",
                                                   "0.0.0.0", "0.0.0.0", "0.0.0.0", "0.0.0.0"};
    const char *macs_comp[BFRT_NUM_COMPUTE_NODE] = {"00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00",
                                                    "00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00",
                                                    "00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00", "00:00:00:00:00:00"};
    const int ports_mem[BFRT_NUM_MEMORY_NODE] = {0, 0, 0, 0};
    const char *ips_mem[BFRT_NUM_MEMORY_NODE] = {"0.0.0.0", "0.0.0.0",
                                                 "0.0.0.0", "0.0.0.0"};
    const char *macs_mem[BFRT_NUM_MEMORY_NODE] = {"00:00:00:00:00:00", "00:00:00:00:00:00",
                                                  "00:00:00:00:00:00", "00:00:00:00:00:00"};

    int get_comp_port(int idx)
    {
      return ports_comp[idx];
    }

    const char *get_comp_ip(int idx)
    {
      return ips_comp[idx];
    }

    const char *get_comp_mac(int idx)
    {
      return macs_comp[idx];
    }

    int get_mem_port(int idx)
    {
      return ports_mem[idx];
    }

    const char *get_mem_ip(int idx)
    {
      return ips_mem[idx];
    }

    const char *get_mem_mac(int idx)
    {
      return macs_mem[idx];
    }
  } // namespace tna_disagg_switch
} // namespace bfrt
