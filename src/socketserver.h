#ifndef SOCKETSERVER_H_
#define SOCKETSERVER_H_

void SocketServer_SetPath(const char *path);
int SocketServer_Enabled(void);
void SocketServer_Poll(void);
void SocketServer_CloseAll(void);
void SocketServer_NotifyStateChanged(void);

#endif /* SOCKETSERVER_H_ */
