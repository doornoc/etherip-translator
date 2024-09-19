#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#define _OPEN_SYS_SOCK_IPV6
#define ETH_FRAME_LEN_ORIG ETH_FRAME_LEN //1514
// #define ETH_FRAME_LEN_ORIG 9216

// コマンドオプション構造体のグローバル変数
static struct option long_options[] = {
   {"internal", required_argument, NULL, 'I'},
   {"external", required_argument, NULL, 'E'},
   {"debug", no_argument, NULL, 'd'},
   {"dump", no_argument, NULL, 'p'},
   {"help", no_argument, NULL, 'h'}
};
// コマンドオプションヘルプ表示
void print_help(char* arg){
   printf("Usage: %s [-I IFNAME] [-E IFNAME] [-dph]\n\n", arg);
   printf("Mikortik独自仕様EtherIPヘッダをRFC準拠のEtherIPヘッダに変換するツール\n\n");
   printf("Mandatory arguments to long options are mandatory for short options too.\n");
   printf("  -I, --internal=IFNAME \t (必須)Mikrotik側インターフェース\n");
   printf("  -E, --external=IFNAME \t (必須)対向側インターフェース\n");
   printf("  -d, --debug \t\t\t デバッグメッセージを表示 デバッグ用\n");
   printf("  -p, --dump \t\t\t パケットダンプを表示 デバッグ用\n");
   printf("  -h, --help \t\t\t このメッセージを表示\n");
   printf("\n");
}

//debugフラグ
int debug_f = 0;
int dump_f = 0;

// 各種メッセージ出力
void die(const char* msg){
   perror(msg);
   exit(1);
}
void debug(const char* msg){
   if(debug_f) fprintf(stderr, " debug: %s\n",msg);
}

// debug用dump
void dump(const uint8_t* p, int len){
   printf(" dst_mac: %02x:%02x:%02x:%02x:%02x:%02x\n", p[0], p[1], p[2], p[3], p[4], p[5]);
   printf(" src_mac: %02x:%02x:%02x:%02x:%02x:%02x\n", p[6], p[7], p[8], p[9], p[10], p[11]);
   // printf(" type: %02x%02x\n", p[12], p[13]);
   int type = (int)p[12]*256 + (int)+p[13];
   printf(" type: 0x%04x / %d\n", type, type);
   if (type == 2048) { //IPv4
      int prot_no = (int)p[23];
      printf(" src_ip: %d.%d.%d.%d\n", p[26], p[27], p[28], p[29]);
      printf(" dst_ip: %d.%d.%d.%d\n", p[30], p[31], p[32], p[33]);
      printf(" protocol: %d\n", prot_no);
   } else if (type == 34525) { //IPv6
      int next_header = (int)p[20];
      printf(" src_ip: ");
      for (size_t i = 0; i < 16; i+=2) {
         if (i != 0) {
            printf(":");
         }
         printf("%x", (int)(p[22+i]*256 + p[22+i+1]));
      }
      printf("\n");
      printf(" dst_ip: ");
      for (size_t i = 0; i < 16; i+=2) {
         if (i != 0) {
            printf(":");
         }
         printf("%x", (int)(p[38+i]*256 + p[38+i+1]));
      }
      printf("\n");
      printf(" next_header: %d\n", next_header);
   }
   for (int i = 0; i < len; i += 16) {
      printf("\t0x%04x: ", i);
      for (int j = 0; j < 16; j++) {
         if (j % 2 == 0) printf(" ");

         if (i + j < len) {
            printf("%02x", p[i + j]);
         } else {
            printf("  ");
         }
      }
      printf("  ");
      for (int j = 0; j < 16; j++) {
         if (i + j >= len) break;
         printf("%c", isprint(p[i + j]) ? p[i + j] : '.');
      }
      printf("\n");
   }
}

// EtherIP書き換え周り ここから
struct addrlist {
   struct addrlist6* ipv6;
   struct addrlist4* ipv4;
};
struct addrlist6 {
   uint8_t addr[16];
   uint8_t eip[2]; //EtherIPヘッダ
   struct addrlist6* next;
};
struct addrlist4 {
   uint8_t addr[4];
   uint8_t eip[8]; //EtherIP(GreTAP)ヘッダ
   struct addrlist4* next;
};
// struct addrlistを作成して初期化
struct addrlist init_list(void){
   // ipv6
   struct addrlist6* list6 = (struct addrlist6*)malloc(sizeof(struct addrlist6));
   if (list6 == NULL) {
      die("malloc list6");
   } else {
      if(debug_f) printf("malloc list6\n");
   }
   for (size_t i = 0; i < 16; i++) {
      list6->addr[i] = 0;
   }
   for (size_t i = 0; i < 2; i++) {
      list6->eip[i] = 0;
   }
   list6->next = NULL;

   // ipv4
   struct addrlist4* list4 = (struct addrlist4*)malloc(sizeof(struct addrlist4));
   if (list4 == NULL) {
      die("malloc list4");
   } else {
      if(debug_f) printf("malloc list4\n");
   }
   for (size_t i = 0; i < 4; i++) {
      list4->addr[i] = 0;
   }
   for (size_t i = 0; i < 2; i++) {
      list6->eip[i] = 0;
   }
   list4->next = NULL;

   // ここからv4v6共通
   struct addrlist list;
   list.ipv6 = list6;
   list.ipv4 = list4;
   return list;
}
// Mikrotikから来たパケットの宛先IPを見て変換リストに追加:IPv6
struct addrlist6* add_list6(struct addrlist6* list6, uint8_t* p){
   debug("add6");
   // 宛先IPが過去に変換したことがあるか確認 あればEtherIPヘッダの内容だけを更新
   for (struct addrlist6* j = list6; j->next != NULL; j = j->next) {
      if (memcmp((void*)p+38, j->addr, sizeof(uint8_t) * 16) == 0) {
         memcpy(list6->eip, &p[54], sizeof(uint8_t) * 2);
         return list6;
      }
   }
   // 宛先IPが過去に変換したことがなければ新規でリストに追加
   debug("add new");
   struct addrlist6* new = (struct addrlist6*)malloc(sizeof(struct addrlist6));
   if (new == NULL) {
      die("malloc new\n");
   }
   memcpy(new->addr, &p[38], sizeof(uint8_t) * 16);
   memcpy(new->eip, &p[54], sizeof(uint8_t) * 2);
   new->next = list6;
   return new;
}
// // 同:IPv4
// struct addrlist6* add_list4(struct addrlist6* list4, uint8_t* p){
//    未実装
// }

// 外部から来たパケットの送信元IPを見て変換リストにあるか確認:IPv6
struct addrlist6* search_list6(struct addrlist6* list6, uint8_t* p){
   debug("search6");
   // 変換リストにあればPass
   for (struct addrlist6* j = list6; j->next != NULL; j = j->next) {
      if (memcmp((void*)p+22, j->addr, sizeof(uint8_t) * 16) == 0) {
         return j;
      }
   }
   // 変換リストになければパケット破棄
   debug("search fail");
   return NULL;
}
// // 同:IPv4
// struct addrlist6* search_list4(struct addrlist4* list4, uint8_t* p){
//    未実装
// }

// mikrotikから出ていくパケットのEtherIPヘッダを書き換え
// return 0: 書き換えせずにパススルー, 1: 書き換え実施, -<n>(負数): キープアライブ対応
int convert_go(uint8_t* p, int len, struct addrlist* list){
   debug("go");
   if (len < 56) return 0; // 56バイト未満のパケットは強制パススルー

   int type = (int)p[12]*256 + (int)+p[13];
   int next_header6 = (int)p[20];
   // int protocol4 = (int)p[23];
   // int gre4_type = (int)p[36]*256 + (int)p[37];

   if (type == 34525 && next_header6 == 97){ // type:34525=IPv6(0x86dd), next_header6:97=EtherIP
      // リスト確認・追加
      list->ipv6 = add_list6(list->ipv6, p);

      // mikrotikはEtherIPでキープアライブを投げているっぽいので、投げ返すようにreturn
      if((len == 60 && p[56] == 0 && p[57] == 0 && p[58] == 0 && p[59] == 0) || len < 60 ) return -6;

      // EtherIPヘッダを書き換え
      p[54] = 48;
      p[55] = 0;
      return 1;
   }
   // else if (type == 2048 && protocol4 == 47 && gre4_type == 25944){ //type:2048(0x0800)=IPv4, protocol4:47=GRE, gre4_type:25944(0x6558)=Transparent_Ethernet_bridgeing(EoGRE/GreTAP)
   //    IPv4は未実装
   // }
   else {
      return 0;
   }

}
// mikrotikに入ってくるパケットのEtherIPヘッダを書き換え
// return 0: 書き換えせずにパススルー, 1: 書き換え実施, -1: パケット破棄
int convert_come(uint8_t* p, int len, struct addrlist* list){
   debug("come");
   if (len < 56) return 0; // 56バイト未満のパケットは強制パススルー

   int type = (int)p[12]*256 + (int)+p[13];
   int next_header6 = (int)p[20];
   // int protocol4 = (int)p[23];
   // int gre4_type = (int)p[36]*256 + (int)p[37];

   if (type == 34525 && next_header6 == 97){ // type:34525=IPv6(0x86dd), next_header6:97=EtherIP
      // リスト確認
      struct addrlist6* new = search_list6(list->ipv6, p);
      if (new == NULL) return -1;

      // EtherIPヘッダを書き換え
      p[54] = new->eip[0];
      p[55] = new->eip[1];
      return 1;
   }
   // else if (type == 2048 && protocol4 == 47 && gre4_type == 25944){ //type:2048(0x0800)=IPv4, protocol4:47=GRE, gre4_type:25944(0x6558)=Transparent_Ethernet_bridgeing(EoGRE/GreTAP)
   //    IPv4は未実装
   // }
   else {
      return 0;
   }


}
// EtherIP書き換え周り ここまで

// インターフェースのバインド部分を外出し
int ifbind(int sock, int ifindex, struct sockaddr_ll* if_sall){
   if (ifindex) {
      /*
      * 受信インタフェースの指定。
      * bindしないと全インタフェースからの受信
      */
      memset(if_sall, 0, sizeof(if_sall));
      if_sall->sll_family = AF_PACKET;
      if_sall->sll_ifindex = ifindex;
      if(bind(sock, (struct sockaddr *) if_sall, sizeof(*if_sall)) < 0) {
         perror("bind");
         close(sock);
         return -1;
      } else {
         return 0;
      }
   } else {
      die("インターフェースのバインドに失敗");
   }
}

// パケット転送部分を外出し
int transfer(int* s_in, int* s_ex, struct sockaddr_ll* if_sall_in, struct sockaddr_ll* if_sall_ex, struct addrlist* list, int flag){
   int len;
   uint8_t buf[ETH_FRAME_LEN_ORIG];
   uint8_t rbuf[ETH_FRAME_LEN_ORIG];

   // パケット受信
   if ((len = recv(*s_in, buf, ETH_FRAME_LEN_ORIG, 0)) < 1) {
      if (errno == EAGAIN) {
         /* まだ来ない。*/
         // printf("madakonai: %d\n", *s_in);
         return 0;
      } else {
         die("recv");
      }
   } else {
      if(debug_f || dump_f) printf("recv %d: len %d\n", *s_in, len);
      if(dump_f) dump(buf, len);

      if (flag == 0) {  // flagが0のとき: mikrotikから出ていく通信(go)
         int n = convert_go(buf, len, list);
         switch(n){
            case 0:
            case 1:
               // 0:変換せず転送, 1:変換して転送 いずれもここで特別な処理はしない。
               break;

            case -6:
               // -6: mikrotikがEtherIPにキープアライブを投げて要るっぽいので投げ返す 逆に転送はしない
               debug("keepalive");
               memcpy(&rbuf[0], &buf[6], sizeof(uint8_t) * 6); //L2 dst mac address!
               memcpy(&rbuf[6], &buf[0], sizeof(uint8_t) * 6);  //L2 src mac address!
               memcpy(&rbuf[12], &buf[12], sizeof(uint8_t) * 2); //L2 type
               memcpy(&rbuf[14], &buf[14], sizeof(uint8_t) * 8); //L3 version ~ hop limit
               memcpy(&rbuf[22], &buf[38], sizeof(uint8_t) * 16); //L3 sec IP addrss!
               memcpy(&rbuf[38], &buf[22], sizeof(uint8_t) * 16); //L3 dst IP addrss!
               memcpy(&rbuf[54], &buf[54], sizeof(uint8_t) * 2); //L4 EtherIP
               memcpy(&rbuf[56], &buf[56], sizeof(uint8_t) * (ETH_FRAME_LEN_ORIG - 56)); //payload

               sendto(*s_in, rbuf, len, 0, (struct sockaddr *)if_sall_in, sizeof(*if_sall_in));
               return len;
         }
      } else {  // flagが1のとき: mikrotikに入ってくる通信 (come)
         if (convert_come(buf, len, list) == -1) return 0;
      }

      // パケット受信
      debug("send");
      sendto(*s_ex, buf, len, 0, (struct sockaddr *)if_sall_ex, sizeof(*if_sall_ex));
      return len;
   }
}

int main(int argc, char *argv[]){
   // コマンドオプションを解析
   char ifname_in[100] = "";
   char ifname_ex[100] = "";
   int opt;
   while((opt = getopt_long(argc, argv, "I:E:dph", long_options, NULL)) != -1) {
      switch(opt) {
         case 'I':
         if (optarg != NULL && strlen(optarg) < sizeof(ifname_in)) {
            strcpy(ifname_in, optarg);
         } else {
            die("bad ifname: internal");
         }
         break;

         case 'E':
         if (optarg != NULL && strlen(optarg) < sizeof(ifname_ex)) {
            strcpy(ifname_ex, optarg);
         } else {
            die("bad ifname: external");
         }
         break;

         case 'd':
         debug_f = 1;
         break;

         case 'p':
         dump_f = 1;
         break;

         case 'h':
         print_help(argv[0]);
         exit(0);
         break;
      }
   }
   if(ifname_in[0] == '\0' || ifname_ex[0] == '\0'){
      print_help(argv[0]);
      if(debug_f) printf("ifname_in: %s\n", ifname_in);
      if(debug_f) printf("ifname_ex: %s\n", ifname_ex);
      fprintf(stderr,"IF名が正しく設定されていません。\n");
      exit(-1);
   }


   // EtherIP変換リストを用意・初期化
   struct addrlist list = init_list();

   // ソケット作成
   int sa = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
   int sb = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
   if (sa < 0) die("socket");
   if (sb < 0) die("socket");

   // インターフェースを指定
   int ifindex_a, ifindex_b;
   struct sockaddr_ll if_sall_a, if_sall_b;
   ifindex_a = if_nametoindex(ifname_in); // mikrotik
   ifindex_b = if_nametoindex(ifname_ex); // external
   if(debug_f) printf("ifindex_a: %d\n", ifindex_a);
   if(debug_f) printf("ifindex_b: %d\n", ifindex_b);
   ifbind(sa, ifindex_a, &if_sall_a);
   ifbind(sb, ifindex_b, &if_sall_b);

   // ソケットをノンブロッキングに変更
   int val = 1;
   ioctl(sa, FIONBIO, &val);
   ioctl(sb, FIONBIO, &val);

   // パケット送受信
   printf("start\n[mikrotik] ==== [%s|%s] ==== [external]\n", ifname_in, ifname_ex);
   while (1) {
      // mikrotik -> external
      transfer(&sa, &sb, &if_sall_a, &if_sall_b, &list, 0);
      // mikrotik <- external
      transfer(&sb, &sa, &if_sall_b, &if_sall_a, &list, 1);
   }

   return 0;
}
