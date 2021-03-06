#include<assert.h>
#include<stdint.h>
#include<string.h>
#include<fcntl.h>
#include<unistd.h>
#include<sys/mman.h>

#include "pcie_uio/generic.h"
#include "pcie_uio/pci.h"
#include "pcie_uio/udmabuf.h"

#include "reg.h"
#include "addr.h"
#include "debug.h"

void debug() {
}
int main(void) {
	// pcie_uio/pci.hで定義
	DevPci dp;
	uint16_t buf16;
	uint32_t buf32;
	uint64_t buf64;

	// _uiofd,_configfdをオープン
	dp.Init();

// PCIe Configuraiton
	// enable  Mastering
	dp.ReadPciReg(dp.kCommandReg, buf16);
	buf16 |= dp.kCommandRegBusMasterEnableFlag;
	dp.WritePciReg(dp.kCommandReg, buf16);

	// BAR0
	uint32_t bar0_default;
	buf32 = 0x0000000c;
	dp.ReadPciReg(dp.kBaseAddressReg0, bar0_default);
	//デフォルトのbar0が0x0000000c;かチェック
	assert(bar0_default == buf32);
	dp.WritePciReg(dp.kBaseAddressReg0, buf32);
	dp.ReadPciReg(dp.kBaseAddressReg0, buf32);
	dp.WritePciReg(dp.kBaseAddressReg0, bar0_default);
	printf("BAR0 %08x\n", bar0_default);


//map dgeneral control registers
	// pcie_uio/mem.hで定義
	Udmabuf mem(1);

	memset(mem.GetVirtPtr<void>(), 0, 1 * 1024 * 1024);

	//map bar0 registers
	int fd = open("/sys/class/uio/uio0/device/resource0", O_RDWR);
	if(fd < 0) {
		perror("open");
		return -1;
	}
	void *addr = mmap(NULL, 1<<18, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(addr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	close(fd);


//Initialization Sequence

	// disable interrupts bywriting to teh EIMC register
	puts("1. Disable interrupts");
	buf32 = 0x7FFFFFFF; //set bits [30:0] , bits 31 is reserved
	WriteReg(addr, RegEimc::kOffset, buf32);
	/* sleep(1);  // unnecessary*/

	// read Status Reg (for print , unnecessary)
	ReadReg(addr, RegStatus::kOffset, buf32);
	printf("Status: %08x\n", buf32);


	// global reset (see 4.6.3.2)
	/*
	initialization sequence の該当項目には
		"Global reset = software reset + link reset"
	と記述あり。
	CTRL.RST(Device reset) には
		"also refferd to as a software reset or global reset"
	と記述あり???
	link reset(set CTRL.LRST)???
	*/
	puts("2.1. device reset () software");
	ReadReg(addr, RegCtrl::kOffset, buf32);
	printf("CTRL: %08x\n", buf32);
	buf32 |= RegCtrl::kFlagDeviceReset;
	WriteReg(addr, RegCtrl::kOffset, buf32);
	//poll the CTRL.RST until it is cleared
	while(1) {
		ReadReg(addr, RegCtrl::kOffset,buf32);
		if(!(buf32 & RegCtrl::kFlagDeviceReset))break;
		usleep(1000); //for mitigating busy wait
	};
	//need  waiting at least 10ms after polling;
	usleep(1100); // wait 10% extra


	// setting flow control (as is not enabled)
	puts("2.2 setting flow control");
	for(int i=0x3200; i<=0x32a0; i+=0x4) {
		((uint32_t*)addr)[i/4] = 0;
	}
	((uint32_t*)addr)[0x3d00/4] = 0;
	for(int i=0x3260; i<0x32a0; i+=0x4) {
		((uint32_t*)addr)[i/4] = 1<<10;
	}

	// link reset
	puts("2.3. link reset");
	ReadReg(addr, RegCtrl::kOffset, buf32);
	buf32 |= RegCtrl::kFlagLinkReset;
	WriteReg(addr, RegCtrl::kOffset, buf32);
	// sleep(1);


	// disable interrupt (see 4.6.3.1)
	puts("disable interrupt (after issuing a global reset)");
	buf32 = 0x7FFFFFFF; //set bits [30:0] , bits 31 is reserved
	WriteReg(addr, RegEimc::kOffset, buf32);
	/* sleep(1);  // unnecessary*/


	// Wait for the NVM auto-read completion.
	puts("3. Wait for the NVM auto-read completion.");
	while(1){
		ReadReg(addr,RegEec::kOffset,buf32);
		if(buf32&RegEec::kFlagAutoRd)break;
		usleep(1000); //for mitigating busy wait
	}



	puts("4. Wait for manegeability configuration done indication");
/**
	while(1){
		ReadReg(addr,RegEemngctl::kOffset,buf32);
		// FIXME: Port0 と 1 どちら(or 両方)を待つべきか判断して判定したい。
		if(buf32 & ( RegEemngctl::kFlagCfgDone0 | RegEemngctl::kFlagCfgDone1))break;
		usleep(1000); //for mitigating busy wait
	}
**/

	puts("5. Wait until DMA initialization complets");
	while(1){
		ReadReg(addr,RegRdrxctl::kOffset,buf32);
		if(buf32 & RegRdrxctl::kFlagDmaidone)break;
		usleep(1000); //for mitigating busy wait
	}


	puts("6. Setup the PHY and the link");
	puts("7. Initialize  all statistical counters");
	puts("8. Initialize receive");
	puts("9. Initialize transmit");
		ReadReg(addr, RegDmatxctl::kOffset, buf32);
		assert(!(buf32 & RegDmatxctl::kFlagTransmitEnable));
		buf32 &= (0xFFFFFFFF ^ (RegDmatxctl::kFlagTransmitEnable));
		printf("DMATXCTL:%08x (TEのみ0であることを確認、他は設定せず)\n",buf32);
		WriteReg(addr, RegDmatxctl::kOffset, buf32);

		ReadReg(addr,  RegDtxtcpflgl::kOffset, buf32);
		printf("DTXTCPFLGL:%08x (設定せず)\n",buf32);

		ReadReg(addr,  RegDtxtcpflgh::kOffset, buf32);
		printf("DTXTCPFLGH:%08x (設定せず)\n",buf32);


		// RTTDCSがない


//		ReadReg(addr, RegDtxmxszrq::kOffset, buf32);
//		printf("DTXMXSZRQ:%08x\n",buf32);

		
//		WriteReg(addr, RegDtxmxszrq::kOffset, buf32);


		// RTTDCSがない



	puts("10. Initialize FCoE");
	puts("11. Initialize Virtualization support");
	puts("12. Configure DCB");
	puts("13. Configure Security");
	puts("14. Enable interrupts");


	// read EEMNGCTL (Manageability EEPROM Mode Control Register)
	// なんで4回読んでる?
	for(int i=0; i<3; i++) {
		uint32_t a;
		a = ((uint32_t*)addr)[0x0000/4];
		printf("CTRL: %08x\n", a);
		a = ((uint32_t*)addr)[0x10110/4];
		printf("EEMNGCTL: %08x\n", (a));
		a = ((uint32_t*)addr)[0x2f00/4];
		printf("RDRXCTL: %08x\n", (a));
		sleep(1);
	}



	 WriteReg(addr, RegFctrl::kOffset, (uint32_t)(RegFctrl::kFlagMulticastEnable | RegFctrl::kFlagUnicastEnable | RegFctrl::kFlagBroadcastEnable));
	 printf("Page BASE Phys = %016lx, Virt = %p\n", mem.GetPhysPtr(), mem.GetVirtPtr<void>());

	 const int descnum = 1 * 8; // 8 entries, must be multiple of 8
	 WriteReg(addr, RegTdba::Offset(0), mem.GetPhysPtr());
	 WriteReg(addr, RegTdlen::Offset(0), (uint32_t)descnum * 16);
	 WriteReg(addr, RegTdh::Offset(0), (uint32_t)0);
	 WriteReg(addr, RegTdt::Offset(0), (uint32_t)0);


	//同じデータを送り続ける。
	//const int rbufsz = 2 * 1024;
	const int rbufsz = 256;
	printf("phys %16lx\n",mem.GetPhysPtr());
	printf("virt %p\n",mem.GetVirtPtr<uint8_t>());

	size_t bufbase = (mem.GetPhysPtr() + descnum * 16 + 2047) / 2048 * 2048;
	uint64_t  databuf = ((uint64_t) mem.GetVirtPtr<uint8_t>() + descnum * 16 + 2047 ) /2048 * 2048;
	printf("bufbase %p\n",(void*)bufbase);
	printf("databuf %p\n",(void*)databuf);
	for(int i=0; i<descnum; i++) {
	 	uint64_t *desc = &mem.GetVirtPtr<uint64_t>()[i * 2];
	 	size_t buf = bufbase ; //+ i * rbufsz;	
		desc[0] = buf;
	 	desc[1] |= 1<<24; //set End of packet 
		desc[1] |= 42;
		printf("RDesc[%d](%p) = %p\n", i,(void*)desc, (void*)buf);
		printf("RDesc[%d](%p) = %llx\n",i,(void *) &desc[1] ,desc[1]);
	
	}
	//databuf = databuf /2048*2048;

	//memset(databuf,0xFF,1024);
	
	char tmp[] = 	"HOGEHOGEHOGEHOGHEOHGOEHGOEHOGHEOjfoisajtwe n aeirnycigsrnisprcntewyiryg peweiuynsvr gpinwnpiuynatrieuoncryapwioueycnipwueiacrpniewvpiewanpyg iweuayprpv ap";

	/*
	char tmp[] = 	"\xFF\xFF\xFF\xFF\xFF\xFF"\ 
			"\x12\x34\x56\x78\x9A\xBC"\ 
	"\x08\x06"\
	"\x00\x01"\
	"\x08\x00"\
	"\x06"\    
	"\x04"\    
	"\x00\x01"\
	"\xfe\x80\x9F\xF2\xE4\x19"\
	"\x01\x02\x04\x04"\
	"\xFF\xFF\xFF\xFF\xFF\xFF"\
	"\xDE\xED\xBE\xEF";
	*/
	memcpy((void *)databuf,tmp,42);


	//buf32 = rbufsz / 1024;
	//WriteReg(addr, RegSrrctl::Offset(0), (uint32_t)0x2);
	//WriteReg(addr, RegRscctl::Offset(0), (uint32_t)0);
	//WriteReg(addr, RegRxdctl::Offset(0), RegRxdctl::kFlagReceiveQueueEnable);

	// while(true) {
	// 	ReadReg(addr, RegRxdctl::Offset(0), buf32);
	// 	if(buf32 & RegRxdctl::kFlagReceiveQueueEnable)
	// 		break;
	// 	__asm__ volatile("" ::: "memory");
	// }
	// WriteReg(addr, RegRdt::Offset(0), (uint32_t)descnum-1);
	// WriteReg(addr, RegRxctrl::kOffset, RegRxctrl::kFlagEnable);

	// enable **all** interrupts
	// WriteReg(addr, RegTdt::Offset(0), (uint32_t)1);
	sleep(1);
	 ReadReg(addr, RegTdh::Offset(0),buf32);
	printf("head %08x\n",buf32);
	 ReadReg(addr, RegTdt::Offset(0),buf32);
	printf("tail %08x\n",buf32);
		ReadReg(addr, RegDmatxctl::kOffset, buf32);
		buf32 |= RegDmatxctl::kFlagTransmitEnable;
		WriteReg(addr, RegDmatxctl::kOffset, buf32);
	sleep(1); 
	puts("enable interrupts");
	 buf32 = 0x7FFFFFFF;
	 WriteReg(addr, RegEims::kOffset, buf32);
	 sleep(1);

	 WriteReg(addr, RegTdt::Offset(0), (uint32_t)1);
	sleep(3);
	 ReadReg(addr, RegTdh::Offset(0),buf32);
	printf("head %08x\n",buf32);

	uint64_t *desc = &mem.GetVirtPtr<uint64_t>()[0 * 2];
	uint64_t tmp64 = desc[1];
	printf("%16llx\n",tmp64);
	for(int i = 63;i>=0;i--){
		if(tmp64 & (1ll<<i))putchar('1');else putchar('0');

	}
	putchar('\n');

	 ReadReg(addr, RegTdh::Offset(0),buf32);
	printf("head %08x\n",buf32);
	 ReadReg(addr, RegTdt::Offset(0),buf32);
	printf("tail %08x\n",buf32);

	return 0;
}
