/*
 * efi.h -- the parts of the UEFI specification (2.10) and of the UEFI
 * Shell specification (2.2) that gork's C library uses, and no more.
 * Tables are complete up to the last member used; what is not used is a
 * placeholder pointer of the right size.  Checked against EDK2's MdePkg.
 */

#ifndef GORK_EFI_H
#define GORK_EFI_H

#include <stddef.h>
#include <stdint.h>

/* x86-64 firmware takes the Microsoft calling convention; arm64 firmware
   the standard one. */
#ifdef __x86_64__
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

typedef uint8_t   BOOLEAN;
typedef uint16_t  CHAR16;
typedef uint64_t  UINTN;
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINTN     EFI_TPL;

#define EFI_ERR(n)            (0x8000000000000000ULL | (n))
#define EFI_ERROR(s)          (((int64_t) (s)) < 0)
#define EFI_SUCCESS           0
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED       EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL  EFI_ERR(5)
#define EFI_NOT_READY         EFI_ERR(6)
#define EFI_DEVICE_ERROR      EFI_ERR(7)
#define EFI_WRITE_PROTECTED   EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES  EFI_ERR(9)
#define EFI_VOLUME_FULL       EFI_ERR(11)
#define EFI_NO_MEDIA          EFI_ERR(12)
#define EFI_NOT_FOUND         EFI_ERR(14)
#define EFI_ACCESS_DENIED     EFI_ERR(15)
#define EFI_NO_MAPPING        EFI_ERR(17)
#define EFI_TIMEOUT           EFI_ERR(18)
#define EFI_NOT_STARTED       EFI_ERR(19)
#define EFI_ABORTED           EFI_ERR(21)
#define EFI_ICMP_ERROR        EFI_ERR(22)
#define EFI_CONNECTION_FIN    EFI_ERR(104)
#define EFI_CONNECTION_RESET  EFI_ERR(105)
#define EFI_CONNECTION_REFUSED EFI_ERR(106)
#define EFI_NO_RESPONSE       EFI_ERR(16)
#define EFI_NETWORK_UNREACHABLE EFI_ERR(100)
#define EFI_HOST_UNREACHABLE  EFI_ERR(101)

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint16_t Year;
    uint8_t  Month, Day, Hour, Minute, Second, Pad1;
    uint32_t Nanosecond;
    int16_t  TimeZone;          /* minutes from UTC, or 2047: local */
    uint8_t  Daylight, Pad2;
} EFI_TIME;

#define EFI_UNSPECIFIED_TIMEZONE 0x07ff

/* ---- console --------------------------------------------------------- */

typedef struct {
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_ESC 0x17

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *,
                               BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *,
                                       EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
                                      CHAR16 *);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- the tables ------------------------------------------------------ */

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *, void *capabilities);
    void *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap,
         *ConvertPointer, *GetVariable, *GetNextVariableName, *SetVariable,
         *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(int type, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

enum { EfiLoaderData = 2 };                     /* EFI_MEMORY_TYPE */
enum { AllHandles, ByRegisterNotify, ByProtocol };

#define EVT_TIMER              0x80000000
#define EVT_NOTIFY_SIGNAL      0x00000200
#define TPL_APPLICATION        4
#define TPL_CALLBACK           8
#define TimerCancel            0
#define TimerPeriodic          1
#define TimerRelative          2

typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT, void *);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL);
    void    (EFIAPI *RestoreTPL)(EFI_TPL);
    void *AllocatePages, *FreePages, *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(int type, UINTN size, void **out);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    EFI_STATUS (EFIAPI *CreateEvent)(uint32_t type, EFI_TPL,
                                     EFI_EVENT_NOTIFY, void *ctx,
                                     EFI_EVENT *out);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT, int type, uint64_t time100ns);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN n, EFI_EVENT *, UINTN *index);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT);
    void *InstallProtocolInterface, *ReinstallProtocolInterface,
         *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify;
    EFI_STATUS (EFIAPI *LocateHandle)(int type, EFI_GUID *, void *key,
                                      UINTN *size, EFI_HANDLE *);
    void *LocateDevicePath, *InstallConfigurationTable, *LoadImage,
         *StartImage;
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
    void *UnloadImage, *ExitBootServices, *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN secs, uint64_t code,
                                          UINTN, CHAR16 *);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE, EFI_HANDLE *drivers,
                                           void *path, BOOLEAN recursive);
    void *DisconnectController, *OpenProtocol,
         *CloseProtocol, *OpenProtocolInformation, *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(int type, EFI_GUID *, void *key,
                                            UINTN *n, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *reg, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
} EFI_SYSTEM_TABLE;

/* ---- files, through the shell ---------------------------------------- */

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_READ_ONLY   0x01
#define EFI_FILE_DIRECTORY   0x10

typedef struct {
    uint64_t Size, FileSize, PhysicalSize;
    EFI_TIME CreateTime, LastAccessTime, ModificationTime;
    uint64_t Attribute;
    CHAR16   FileName[1];
} EFI_FILE_INFO;

typedef void *SHELL_FILE_HANDLE;

typedef struct EFI_LIST_ENTRY {
    struct EFI_LIST_ENTRY *Flink, *Blink;
} EFI_LIST_ENTRY;

typedef struct {
    EFI_LIST_ENTRY    Link;
    EFI_STATUS        Status;
    const CHAR16     *FullName;
    const CHAR16     *FileName;
    SHELL_FILE_HANDLE Handle;
    EFI_FILE_INFO    *Info;
} EFI_SHELL_FILE_INFO;

#define EFI_SHELL_PROTOCOL_GUID \
    { 0x6302d008, 0x7f9b, 0x4f30, \
      { 0x87, 0xac, 0x60, 0xc9, 0xfe, 0xf5, 0xda, 0x4e } }
#define EFI_SHELL_PARAMETERS_PROTOCOL_GUID \
    { 0x752f3136, 0x4e16, 0x4fdc, \
      { 0xa2, 0x2a, 0xe5, 0xf4, 0x68, 0x12, 0xf4, 0xca } }

typedef struct {
    EFI_STATUS (EFIAPI *Execute)(EFI_HANDLE *parent, CHAR16 *cmdline,
                                 CHAR16 **env, EFI_STATUS *status);
    const CHAR16 *(EFIAPI *GetEnv)(const CHAR16 *name);
    EFI_STATUS (EFIAPI *SetEnv)(const CHAR16 *name, const CHAR16 *value,
                                BOOLEAN isvolatile);
    void *GetAlias, *SetAlias, *GetHelpText, *GetDevicePathFromMap,
         *GetMapFromDevicePath, *GetDevicePathFromFilePath,
         *GetFilePathFromDevicePath, *SetMap;
    const CHAR16 *(EFIAPI *GetCurDir)(const CHAR16 *map);
    void *SetCurDir, *OpenFileList;
    EFI_STATUS (EFIAPI *FreeFileList)(EFI_SHELL_FILE_INFO **);
    void *RemoveDupInFileList, *BatchIsActive, *IsRootShell,
         *EnablePageBreak, *DisablePageBreak, *GetPageBreak, *GetDeviceName;
    EFI_FILE_INFO *(EFIAPI *GetFileInfo)(SHELL_FILE_HANDLE);
    EFI_STATUS (EFIAPI *SetFileInfo)(SHELL_FILE_HANDLE,
                                     const EFI_FILE_INFO *);
    EFI_STATUS (EFIAPI *OpenFileByName)(const CHAR16 *, SHELL_FILE_HANDLE *,
                                        uint64_t mode);
    EFI_STATUS (EFIAPI *CloseFile)(SHELL_FILE_HANDLE);
    EFI_STATUS (EFIAPI *CreateFile)(const CHAR16 *, uint64_t attribs,
                                    SHELL_FILE_HANDLE *);
    EFI_STATUS (EFIAPI *ReadFile)(SHELL_FILE_HANDLE, UINTN *, void *);
    EFI_STATUS (EFIAPI *WriteFile)(SHELL_FILE_HANDLE, UINTN *, void *);
    EFI_STATUS (EFIAPI *DeleteFile)(SHELL_FILE_HANDLE);
    EFI_STATUS (EFIAPI *DeleteFileByName)(const CHAR16 *);
    EFI_STATUS (EFIAPI *GetFilePosition)(SHELL_FILE_HANDLE, uint64_t *);
    EFI_STATUS (EFIAPI *SetFilePosition)(SHELL_FILE_HANDLE, uint64_t);
    EFI_STATUS (EFIAPI *FlushFile)(SHELL_FILE_HANDLE);
    void *FindFiles;
    EFI_STATUS (EFIAPI *FindFilesInDir)(SHELL_FILE_HANDLE,
                                        EFI_SHELL_FILE_INFO **);
    EFI_STATUS (EFIAPI *GetFileSize)(SHELL_FILE_HANDLE, uint64_t *);
    void *OpenRoot, *OpenRootByHandle;
    EFI_EVENT ExecutionBreak;               /* Ctrl-C */
    uint32_t  MajorVersion, MinorVersion;
} EFI_SHELL_PROTOCOL;

typedef struct {
    CHAR16          **Argv;
    UINTN             Argc;
    SHELL_FILE_HANDLE StdIn, StdOut, StdErr;
} EFI_SHELL_PARAMETERS_PROTOCOL;

/* ---- the network ----------------------------------------------------- */

typedef struct { uint8_t Addr[4]; } EFI_IPv4_ADDRESS;
typedef struct { uint8_t Addr[32]; } EFI_MAC_ADDRESS;

typedef struct EFI_SERVICE_BINDING_PROTOCOL {
    EFI_STATUS (EFIAPI *CreateChild)(struct EFI_SERVICE_BINDING_PROTOCOL *,
                                     EFI_HANDLE *);
    EFI_STATUS (EFIAPI *DestroyChild)(struct EFI_SERVICE_BINDING_PROTOCOL *,
                                      EFI_HANDLE);
} EFI_SERVICE_BINDING_PROTOCOL;

#define EFI_IP4_CONFIG2_PROTOCOL_GUID \
    { 0x5b446ed1, 0xe30b, 0x4faa, \
      { 0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80 } }

enum {                                  /* EFI_IP4_CONFIG2_DATA_TYPE */
    Ip4Config2DataTypeInterfaceInfo,
    Ip4Config2DataTypePolicy,
    Ip4Config2DataTypeManualAddress,
    Ip4Config2DataTypeGateway,
    Ip4Config2DataTypeDnsServer
};
enum { Ip4Config2PolicyStatic, Ip4Config2PolicyDhcp };

typedef struct {
    CHAR16           Name[32];
    uint8_t          IfType;
    uint32_t         HwAddressSize;
    EFI_MAC_ADDRESS  HwAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    uint32_t         RouteTableSize;
    void            *RouteTable;
} EFI_IP4_CONFIG2_INTERFACE_INFO;

typedef struct EFI_IP4_CONFIG2_PROTOCOL {
    EFI_STATUS (EFIAPI *SetData)(struct EFI_IP4_CONFIG2_PROTOCOL *, int type,
                                 UINTN size, void *data);
    EFI_STATUS (EFIAPI *GetData)(struct EFI_IP4_CONFIG2_PROTOCOL *, int type,
                                 UINTN *size, void *data);
} EFI_IP4_CONFIG2_PROTOCOL;

#define EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID \
    { 0x00720665, 0x67eb, 0x4a99, \
      { 0xba, 0xf7, 0xd3, 0xc3, 0x3a, 0x1c, 0x7c, 0xc9 } }
#define EFI_TCP4_PROTOCOL_GUID \
    { 0x65530bc7, 0xa359, 0x410f, \
      { 0xb0, 0x10, 0x5a, 0xad, 0xc7, 0xec, 0x2b, 0x62 } }

typedef struct {
    BOOLEAN          UseDefaultAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    uint16_t         StationPort;
    EFI_IPv4_ADDRESS RemoteAddress;
    uint16_t         RemotePort;
    BOOLEAN          ActiveFlag;
} EFI_TCP4_ACCESS_POINT;

typedef struct {
    uint8_t               TypeOfService;
    uint8_t               TimeToLive;
    EFI_TCP4_ACCESS_POINT AccessPoint;
    void                 *ControlOption;
} EFI_TCP4_CONFIG_DATA;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
} EFI_TCP4_COMPLETION_TOKEN;

typedef struct {
    uint32_t FragmentLength;
    void    *FragmentBuffer;
} EFI_TCP4_FRAGMENT_DATA;

typedef struct {
    BOOLEAN                UrgentFlag;
    uint32_t               DataLength;
    uint32_t               FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_RECEIVE_DATA;

typedef struct {
    BOOLEAN                Push;
    BOOLEAN                Urgent;
    uint32_t               DataLength;
    uint32_t               FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_TRANSMIT_DATA;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    union {
        EFI_TCP4_RECEIVE_DATA  *RxData;
        EFI_TCP4_TRANSMIT_DATA *TxData;
    } Packet;
} EFI_TCP4_IO_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    BOOLEAN                   AbortOnClose;
} EFI_TCP4_CLOSE_TOKEN;

typedef struct EFI_TCP4_PROTOCOL {
    void *GetModeData;
    EFI_STATUS (EFIAPI *Configure)(struct EFI_TCP4_PROTOCOL *,
                                   EFI_TCP4_CONFIG_DATA *);
    void *Routes;
    EFI_STATUS (EFIAPI *Connect)(struct EFI_TCP4_PROTOCOL *,
                                 EFI_TCP4_COMPLETION_TOKEN *);
    void *Accept;
    EFI_STATUS (EFIAPI *Transmit)(struct EFI_TCP4_PROTOCOL *,
                                  EFI_TCP4_IO_TOKEN *);
    EFI_STATUS (EFIAPI *Receive)(struct EFI_TCP4_PROTOCOL *,
                                 EFI_TCP4_IO_TOKEN *);
    EFI_STATUS (EFIAPI *Close)(struct EFI_TCP4_PROTOCOL *,
                               EFI_TCP4_CLOSE_TOKEN *);
    EFI_STATUS (EFIAPI *Cancel)(struct EFI_TCP4_PROTOCOL *,
                                EFI_TCP4_COMPLETION_TOKEN *);
    EFI_STATUS (EFIAPI *Poll)(struct EFI_TCP4_PROTOCOL *);
} EFI_TCP4_PROTOCOL;

#define EFI_DNS4_SERVICE_BINDING_PROTOCOL_GUID \
    { 0xb625b186, 0xe063, 0x44f7, \
      { 0x89, 0x05, 0x6a, 0x74, 0xdc, 0x6f, 0x52, 0xb4 } }
#define EFI_DNS4_PROTOCOL_GUID \
    { 0xae3d28cc, 0xe05b, 0x4fa1, \
      { 0xa0, 0x11, 0x7e, 0xb5, 0x5a, 0x3f, 0x14, 0x01 } }

typedef struct {
    UINTN             DnsServerListCount;
    EFI_IPv4_ADDRESS *DnsServerList;
    BOOLEAN           UseDefaultSetting;
    BOOLEAN           EnableDnsCache;
    uint8_t           Protocol;             /* 17: UDP */
    EFI_IPv4_ADDRESS  StationIp;
    EFI_IPv4_ADDRESS  SubnetMask;
    uint16_t          LocalPort;
    uint32_t          RetryCount;
    uint32_t          RetryInterval;        /* seconds */
} EFI_DNS4_CONFIG_DATA;

typedef struct {
    uint32_t          IpCount;
    EFI_IPv4_ADDRESS *IpList;
} DNS_HOST_TO_ADDR_DATA;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
    uint32_t   RetryCount;
    uint32_t   RetryInterval;
    union {
        DNS_HOST_TO_ADDR_DATA *H2AData;
        void                  *Other;
    } RspData;
} EFI_DNS4_COMPLETION_TOKEN;

typedef struct EFI_DNS4_PROTOCOL {
    void *GetModeData;
    EFI_STATUS (EFIAPI *Configure)(struct EFI_DNS4_PROTOCOL *,
                                   EFI_DNS4_CONFIG_DATA *);
    EFI_STATUS (EFIAPI *HostNameToIp)(struct EFI_DNS4_PROTOCOL *, CHAR16 *,
                                      EFI_DNS4_COMPLETION_TOKEN *);
    void *IpToHostName, *GeneralLookUp, *UpdateDnsCache;
    EFI_STATUS (EFIAPI *Poll)(struct EFI_DNS4_PROTOCOL *);
    EFI_STATUS (EFIAPI *Cancel)(struct EFI_DNS4_PROTOCOL *,
                                EFI_DNS4_COMPLETION_TOKEN *);
} EFI_DNS4_PROTOCOL;

/* The network card itself, for ip.c: whole Ethernet frames. */
#define EFI_SIMPLE_NETWORK_PROTOCOL_GUID \
    { 0xa19832b9, 0xac25, 0x11d3, \
      { 0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

enum { EfiSimpleNetworkStopped, EfiSimpleNetworkStarted,
       EfiSimpleNetworkInitialized };
#define EFI_SIMPLE_NETWORK_RECEIVE_UNICAST   0x01
#define EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST 0x04

typedef struct {
    uint32_t        State;
    uint32_t        HwAddressSize;
    uint32_t        MediaHeaderSize;
    uint32_t        MaxPacketSize;
    uint32_t        NvRamSize;
    uint32_t        NvRamAccessSize;
    uint32_t        ReceiveFilterMask;
    uint32_t        ReceiveFilterSetting;
    uint32_t        MaxMCastFilterCount;
    uint32_t        MCastFilterCount;
    EFI_MAC_ADDRESS MCastFilter[16];
    EFI_MAC_ADDRESS CurrentAddress;
    EFI_MAC_ADDRESS BroadcastAddress;
    EFI_MAC_ADDRESS PermanentAddress;
    uint8_t         IfType;
    BOOLEAN         MacAddressChangeable;
    BOOLEAN         MultipleTxSupported;
    BOOLEAN         MediaPresentSupported;
    BOOLEAN         MediaPresent;
} EFI_SIMPLE_NETWORK_MODE;

typedef struct EFI_SIMPLE_NETWORK_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Start)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (EFIAPI *Stop)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (EFIAPI *Initialize)(struct EFI_SIMPLE_NETWORK_PROTOCOL *,
                                    UINTN extra_rx, UINTN extra_tx);
    void *Reset, *Shutdown;
    EFI_STATUS (EFIAPI *ReceiveFilters)(struct EFI_SIMPLE_NETWORK_PROTOCOL *,
                                        uint32_t enable, uint32_t disable,
                                        BOOLEAN reset_mcast, UINTN n,
                                        EFI_MAC_ADDRESS *mcast);
    void *StationAddress, *Statistics, *MCastIpToMac, *NvData;
    EFI_STATUS (EFIAPI *GetStatus)(struct EFI_SIMPLE_NETWORK_PROTOCOL *,
                                   uint32_t *irq, void **txbuf);
    EFI_STATUS (EFIAPI *Transmit)(struct EFI_SIMPLE_NETWORK_PROTOCOL *,
                                  UINTN header_size, UINTN size, void *buf,
                                  EFI_MAC_ADDRESS *src, EFI_MAC_ADDRESS *dst,
                                  uint16_t *proto);
    EFI_STATUS (EFIAPI *Receive)(struct EFI_SIMPLE_NETWORK_PROTOCOL *,
                                 UINTN *header_size, UINTN *size, void *buf,
                                 EFI_MAC_ADDRESS *src, EFI_MAC_ADDRESS *dst,
                                 uint16_t *proto);
    EFI_EVENT                WaitForPacket;
    EFI_SIMPLE_NETWORK_MODE *Mode;
} EFI_SIMPLE_NETWORK_PROTOCOL;

/* ---- for the rest of the library (libc.c, net.c) --------------------- */

extern EFI_HANDLE          efi_image;
extern EFI_SYSTEM_TABLE   *efi_st;
extern EFI_BOOT_SERVICES  *efi_bs;
extern EFI_SHELL_PROTOCOL *efi_shell;

/* errno for a failed EFI call */
int efi_errno(EFI_STATUS s);
/* UTF-8 to a CHAR16 string, malloc'd (NULL if out of memory); with
   backslashes for slashes if `path`. */
CHAR16 *efi_wide(const char *s, int path);
/* CHAR16 to UTF-8, malloc'd. */
char   *efi_narrow(const CHAR16 *s);
/* A socket's descriptor is ready to read (or at its end): for select(). */
int     efi_sock_ready(int fd);
int     efi_sock_read(int fd, void *p, size_t n);
int     efi_sock_write(int fd, const void *p, size_t n);
int     efi_sock_close(int fd);
/* Let the network stack run (net.c), from anywhere that waits. */
void    efi_net_poll(void);

/* ../ip.c: gork's own TCP/IP on the network card, for firmware without
   TCP4; card.c is its card. */
#include "../ip.h"

#endif /* GORK_EFI_H */
