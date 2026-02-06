#ifndef REMOTEMONITOR_H_
#define REMOTEMONITOR_H_

int RemoteMonitor_SetTransport(const char *transport);
const char *RemoteMonitor_GetTransport(void);
void RemoteMonitor_SetSocketPath(const char *path);
const char *RemoteMonitor_GetSocketPath(void);
const char *RemoteMonitor_DefaultSocketPath(void);
void RemoteMonitor_EnableDefault(void);
void RemoteMonitor_Disable(void);
int RemoteMonitor_Enabled(void);
int RemoteMonitor_HasClients(void);
void RemoteMonitor_Poll(void);
void RemoteMonitor_CloseAll(void);
void RemoteMonitor_NotifyStateChanged(void);

#endif /* REMOTEMONITOR_H_ */
