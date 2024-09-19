# EtherIP-translator
Mikrotik独自のEoIPパケットのヘッダと、RFC3378準拠のEtherIPヘッダを相互に変換して、NEC UNIVERGE IXシリーズなどとMikrotikルータでEtherIPを相互接続できるようにするツールです。

## 使用方法
### ビルド
gccなどでコンパイルします。
```
% gcc main.c -o etherip-tranlator.exe
```
make対応は気が向いたらやります。
### 起動
NICを二つ以上持ったサーバを用意し、片方をMikrotikと繋がる側に、もう片方を対向機器(the InternetやNGNなど)側に接続します。
```
[Mikrotikルータ] ==== [サーバ] ==== [the Internet / NGN / etc.]
```
サーバでツールを起動します。管理者権限が必要です。
```
% sudo ./etherip-translator.exe -I [Mikrotik側インターフェース] -E [対向側インターフェース]
```

その他オプションは下記のとおり
```
% ./etherip-translator.exe -h
Usage: ./trans6.exe [-I IFNAME] [-E IFNAME] [-dph]

Mikortik独自仕様EtherIPヘッダをRFC準拠のEtherIPヘッダに変換するツール

Mandatory arguments to long options are mandatory for short options too.
  -I, --internal=IFNAME          (必須)Mikrotik側インターフェース
  -E, --external=IFNAME          (必須)対向側インターフェース
  -d, --debug                    デバッグメッセージを表示 デバッグ用
  -p, --dump                     パケットダンプを表示 デバッグ用
  -h, --help                     このメッセージを表示
```

## やっていること
Mikrotikルータが出すEoIPパケット(IPv6 Next-header:97)の当該ヘッダ部分をRFC準拠のヘッダに書き換えて送ります。

逆も然りです。

これが

<img width="1919" alt="image" src="https://github.com/user-attachments/assets/d48eb533-fdae-402a-a6bd-9753c89fb421">

こうなる

<img width="1920" alt="image" src="https://github.com/user-attachments/assets/8783f48a-636a-4edc-93b0-4669d3440eac">

それ以外のパケットはパススルーします。

それだけ


...だったらどれだけ楽だったことか。

### 他にやってること

1. Tunnel ID

MikrotikのEoIPにはTunnel IDという値があり、同じ値を重複しては設定できないようになっています。

この値は同一IPで複数トンネルを張る際に識別する際に使用しているみたいです。

![image](https://github.com/user-attachments/assets/4d3a5a93-aa26-4922-b367-93d2e41dc458)

このIDはEtherIPヘッダのReserved領域の末尾を活用して表しているように見受けられます。

(GREでいうkeyオプションみたいですね)

![image](https://github.com/user-attachments/assets/956a7a68-f470-428c-9314-edfdc187fa64)

ただ、RFC準拠のEtherIPではこのReserved領域はすべて0である必要があるので、ここも変換する必要があります。

さて、問題はMikrotikルータに戻ってくるパケットです。

MikrotikルータはTunnel IDで識別してくるので、Tunnel対向に適切なIDをつけて戻してあげる必要があります。

つまり、Tunnel対向とIDの組み合わせを覚えさせる必要があるということです。

いろいろやり方はあると思いますが、本ツールではMikrotikから出てきたEtherIPパケットを見て組み合わせを記憶し、戻ってきたパケットにも適用するようにしました。記憶する前に対向からパケットが届いてしまった場合はそのパケットを破棄します。

この仕様の関係で、同一対向で複数のトンネルを張ることはできなくなっていますが、ご了承ください。

同一対向でトンネルを張った場合、新しい方(後からパケットが届いた方)で処理されます。


2. keepalive


MikrotikルータのEoIPにはkeepaliveの機能があり、Tunnel対向が生きているかを相互監視しているみたいです。

![image](https://github.com/user-attachments/assets/b14400cb-4b81-46ff-b562-1f7dc579d2e0)

この機能を止めることはできず、また対向からパケットが届かないとリンクダウン状態となるため無視することもできない仕様だったりします。

そこで、本ツールではMikrotikからkeepaliveパケットが届いたら、内容をそっくりそのまま投げ返すことで誤魔化す機能を持たせました。

この仕様の関係でkeepaliveが本来の役目を果たさなくなってしまいますが、ご了承ください。(本ツールの死活監視程度には活用できます)

## その他、既知の問題点
- 現状、IPv4には対応していません。(IPv4ではEtherIPではなくGRETAPをベースに改変したヘッダを使用しているらしいですが、対応が追い付いていません。)
- 無限ループを使用している関係で本ツール起動中はCPUの1コアが常に使用率100%になりますが、仕様です。
