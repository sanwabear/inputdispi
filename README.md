# input_dispi 説明書

## 概要

`input_dispi` は、Raspberry Pi 上で、キーボードからの入力を取得し、1920x1080 60FPS で表示するフレーム表示アプリケーションです。

配信用ビデオミキサー上のバックストリームキー合成用に作成したもので、高精度のキーディスプレイを目指して作成しました。

JAMMA to xinputであるJAMMAアダプタを対象にしたマッピングとしています。

| 入力 | 1Pマッピング | 2Pマッピング |
|:---|:---|:---|
| 上 | W | ↑ |
| 下 | D | ↓ |
| 左 | A | ← |
| 右 | D | → |
| Aボタン | N | NUMパッド1 |
| Bボタン | M | NUMパッド2 |
| Cボタン | , | NUMパッド3 |
| Dボタン | . | NUMパッド4 |
| スタートボタン | 1 | 2 |
| コイン(セレクト)ボタン | 5 | 6 |

## 使用機器

JAMMA to xpinputアダプタ: Aliexpressなどで購入できます。
写真ではCHAMMA接続にならないように絶縁しています。
![JAMMA to xpinput](/doc/adapter.jpg)

キーボードモードでの利用を想定しているのでスイッチをセットします。
![Keyboad Switch](/doc/adapter.jpg)

## 利用イメージ

本ツールだけの出力は黒背景の表示になります。
![Tool only](/doc/bsk1.png)

バックストリームキー合成後のイメージです。
![BSK](/doc/bsk2.png)

## インストール方法

### 1. 必要パッケージを導入

```bash
chmod +x *.sh
./setup.sh
```

#### 1.1 内部フレームレートの変更

ソースでは NEOGEO MVS の59.1856Hzにあわせた設定にしています。

必要であれば、希望の環境にあわせてソースの該当箇所を変更してください。

デフォルト - MVS用(59.1856Hz)
```c
    struct timespec interval = INTERVAL_MVS;
```

AES用(59.599Hz)
```c
    struct timespec interval = INTERVAL_AES;
```

60Hz
```c
    struct timespec interval = INTERVAL_60;
```

### 2. ソースからビルドとインストール

```bash
./mk.sh
```

### 3. 単発起動・終了

```bash
./run.sh
./sig.sh
```

### 3. サービス登録

```bash
./svc.sh
```

## 予備機能

スタート+セレクト同時押しで左上にFPSが表示されます。

再度スタート+セレクト同時押しで表示を消せます。

## 使用ライブラリ等

- ライブラリ: raylib
- フォント: orkney-mediumで矢印を改造して利用
