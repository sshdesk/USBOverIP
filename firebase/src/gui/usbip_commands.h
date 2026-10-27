#ifndef USBIP_COMMANDS_H
#define USBIP_COMMANDS_H

#include <string>
#include <vector>

int usbipCLI(int argc, wchar_t *argv[]);
void execute_usbip_command(const std::vector<std::string>& args_utf8);
int InitializeUsbIpClientTunnelSession();
void ShutdownUsbIpClientTunnel();

#endif // USBIP_COMMANDS_H