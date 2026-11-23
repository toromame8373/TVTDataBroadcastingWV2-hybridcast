# TVTDataBroadcastingWV2 ハイブリッドキャスト対応版

元: https://github.com/otya128/TVTDataBroadcastingWV2 (コミット 70f0e2ec84fa2f26d0989248c15500ad9b9bba1d)

## ビルド (Windows) — Node.js / npm は不要
1. Visual Studio 2022 (「C++によるデスクトップ開発」) を入れておく(Node.js と npm は **不要**)
2. zip を展開し、**`build.bat` をダブルクリック**(32bitは `build.bat x86`)
3. 成功すると `out\Plugins\` ができる。次の「フォルダ構成」のとおりに、TVTest の `Plugins` フォルダへコピーする

ブラウザ側のJavaScript(データ放送とハイブリッドキャストの処理)は、**ビルド済みのものを `prebuilt\dist` に同梱**しています。
`build.bat` は、MSBuildの検索 → NuGetパッケージの復元(WebView2など。ネットワークが必要) → プラグインのビルド →
出力の収集、だけを行います。どこかで失敗したら原因を表示して止まります(コマンドを1行につなげる書き方は使っていません)。

- `prebuilt\BUILD_INFO.txt` に、同梱ファイルのSHA-256があります。`prebuilt\THIRD_PARTY_NOTICES.txt` に、同梱したnpmパッケージ(MIT / Apache-2.0)の
  ライセンスがあります(ビルドで `out` にもコピーされます)
- **TypeScript(`browser\src`)を自分で変更したときだけ**、`rebuild-browser.bat`(こちらは Node.js が必要)で `prebuilt\dist` を作り直します。
  そのあと `build.bat` を実行してください。`build.bat` は、`browser\src` からJavaScriptを作り直しません

## フォルダ構成 (TVTest の Plugins フォルダの中)
`out\Plugins\` の中身を、**すべて**、TVTest の `Plugins` フォルダへ上書きコピーします(`.tvtp` だけでは、JavaScriptが古いままになります)。

```
TVTest\Plugins\
  TVTDataBroadcastingWV2.tvtp               ... プラグイン本体(ビルドで作られる)
  TVTDataBroadcastingWV2.ini                ... 設定。.ini.sample をコピーして名前を変え、EnableNetwork=1 を確認する
  TVTDataBroadcastingWV2.ini.sample         ... 設定のひな形(ビルドで作られる)
  TVTDataBroadcastingWV2\
    resources\
      TVTDataBroadcastingWV2.html           ... (ビルドで作られる)
      dist\                                 ... TVTDataBroadcastingWV2.js、hybridcast_api.js、フォント(*.woff2)(ビルドで作られる)
    WebView2Data\                           ... 実行すると自動で作られる。BMLの保存データなどが入るので、消さない
    hybridcast.log                          ... 実行すると自動で作られる(ハイブリッドキャストのログ)
    hybridcast.old.log                      ... ログが2MBを超えると、前のログがここへ退避される(1世代)
    WebView2\                               ... (任意)Fixed Version のWebView2ランタイムを置く場合だけ
```
TVTestを起動したまま上書きしないでください(必ず終了してからコピーします)。

## ログファイル (hybridcast.log)
`Plugins\TVTDataBroadcastingWV2\hybridcast.log` に、1行1件のUTF-8で追記されます。メモ帳で読めます。
**どのサイトへ誘導されたか、なぜアプリが終了したか、エラー** を、次の内容で記録します。

- 起動要求(起動URL、組織識別、アプリ識別)、送ったUser-Agent、起動に失敗した理由(通信が無効、スクリプトが読めない など)
- アプリの文書の遷移(フレームも含む)。リダイレクトか、操作による遷移かも記録します。**バウンダリ外への遷移は WARN で、遷移先のURLつき**
- HTTPのエラー(403 など)と、リダイレクト先(Location)
- アプリの読み込みの成功・失敗(webErrorStatus)
- アプリのJavaScriptの例外と `console.error`
- アプリからの要求(`destroyApplication`、`exitFromManagedState` の行き先URL、`launchDataBroadcastingBrowser` など)と、拒否したもの
- データ放送側(BML)の記録: AITの取得失敗、AITに該当アプリがなかったとき(AITに何があったか)
- アプリを終了した理由(選局、`destroyApplication`、バウンダリ外への遷移)

WARN と ERROR は、TVTestのログにも出ます。

記録の例(動作の説明用で、実際の出力ではありません):
```
2026-10-24 21:47:12.345 [INFO] [BML] startAITControlledApp: 組織識別=10 アプリ識別=1 http://hc.nhk.or.jp/p/nhkp.ait
2026-10-24 21:47:12.401 [INFO] 起動要求: http://hc.nhk.or.jp/p/index.html (組織識別=16 アプリ識別=1)
2026-10-24 21:47:12.402 [INFO] User-Agent: Mozilla/5.0 ... Hybridcast/1.0 (HD,HD;000000;TVTDataBroadcastingWV2;1;0;)
2026-10-24 21:47:12.430 [INFO] 遷移 http://hc.nhk.or.jp/p/index.html
2026-10-24 21:47:12.610 [INFO] HTTP 302 http://hc.nhk.or.jp/p/index.html -> https://example.invalid/unsupported
2026-10-24 21:47:12.611 [WARN] バウンダリ外への遷移[リダイレクト] https://example.invalid/unsupported -> アプリを終了します
2026-10-24 21:47:12.640 [INFO] アプリを終了しました: 放送受信映像のみの状態に戻ります
```

**個人情報への配慮**: URLのクエリ(`?` 以降)とフラグメント、`ユーザー名:パスワード@` は、既定では記録しません(`?(省略)` と表示)。
iniの `HybridcastLogFullUrl=1` で、クエリも記録します。ログを他人に渡すときは、内容を確認してください。
`HybridcastLog=0` で、ログを書かなくなります。1回の起動での行数は3000行までで、超えると省略します。

## 仕様(IPTVFJ STD-0010 / 0011 / 0013)に合わせた点
- **バウンダリの判定(STD-0013 表 6-1)**: ドメインだけで書かれた管理URL(例 `http://nhk.or.jp`)は、下位のドメイン
  (`hc.nhk.or.jp` など)も含む。パスを書いた場合は、ドメインを厳格に解釈する(下位ドメインを含まない)。
  ドメイン直後のスラッシュだけ(`http://nhk.or.jp/`)も、パスの指定に当たる。スキーマ(http/https)は一致が必要。
  権限は、最長一致(最も階層の深い)バウンダリのものが、記述順によらず適用される。
  実際のNHKのAIT(管理URLがドメインだけ)で、以前の前方一致の判定では起動URLがバウンダリ外になる不具合を見つけて直した
- **バウンダリ**: 制限するのは文書(HTML)の遷移だけ(iframeを含む)。画像・スクリプト・通信などの部品は制限しない。
  バウンダリ外への文書遷移を検出したら、アプリを即時終了して「放送受信映像のみ」にする(STD-0013 8.1.3.10.2)
- **終了後の状態**: `destroyApplication` とバウンダリ外遷移は「放送受信映像のみ」(データ放送は裏で動き、リモコン操作で戻る)。
  `launchDataBroadcastingBrowser` はデータ放送へ。選局で別のサービスになったらアプリを終了する
- **exitFromManagedState**: 一般アプリを、拡張APIなし(アンマネージド)で開く。映像・音声は直ちに止める(STD-0013 8.1.3.10.1)
- **権限**: 権限が 0x1fff(全許可)でない文書では、`getOwnerApplication` / `destroyApplication` / `hasCapability` 以外の
  拡張APIと放送映像音声オブジェクトを使えない(STD-0013 表 8-3)。使うと `NOT_AUTHORIZED_ERR` の例外(表 8-4/8-5)
- **API検査**: 引数が不正なら `INVALID_PARAM_ERR`。`getOwnerApplication(document)` は、別アプリの文書なら null
- **addPermissionManagedArea**: バウンダリを広げられる。アプリ終了時に初期化(STD-0011 3.1.3.3)
- **放送映像音声オブジェクト**: `param` 要素による初期設定に対応。実際にできないことは成功を偽らない
  (別ストリームの選択・字幕は `false`)。ミュートはTVTestの音声に反映される
- **放送映像の穴**: 放送映像音声オブジェクトの場所は、`visibility: hidden` で透明な穴にする。
  何もしないと、Chromium(WebView2)が「このプラグインはサポートされていません」という代替表示で、その場所を覆ってしまう
- **User-Agent**: アプリへの通信に `Hybridcast/1.0 (HD,HD;<MakerId>;<ModelId>;1;0;)` を付ける(STD-0013 8.1.4.1.1.8)。
  MakerId は IEEE OUI(6桁の16進)で、既定は `000000`(他社のIDは名乗らない)。ini の `HybridcastMakerId` / `HybridcastModelId` で変更できる
- **getBrowserSupport**: 実際の局(NHK)が使う `AITTransportMethod`(Transport)の綴りにも答える

- **BMLからの起動の引数**: 実際のNHKのBMLは、AITに `orgId 16` と書かれたアプリを `startAITControlledApp(10, 1, ...)` で呼ぶ
  (0x10 = 16。16進の桁をそのまま数値として渡している)。10進として完全一致するものを先に探し、無ければ16進として読み替える。
  (ARIB STD-B24 第二編 7.6.20 は未入手のため、実際の呼び出しからの推測)

## 見直して直した不具合
- 外部ページ(アプリ)が、内部プロキシ(通信先を問わず取得できる)や、プラグインの仮想ホストに触れられた → 遮断
- 外部ページから送られた不正なJSONや型違いのメッセージで、プロセスが落ちる可能性があった → 例外を投げない解析に変更
- 外部ページが `tune` などのプラグイン用メッセージを送れた → アプリ用メッセージだけに限定
- ポップアップ(window.open)が無制限に開けた → 遮断
- 起動処理中に選局すると、別サービスでアプリが開く競合 → 中止するようにした
- AITに不正なアプリが1件あると、全体が捨てられた → 不正な1件だけ無視

## まだできていないこと / 確認できていないこと
- **WebView2 SDK**: プロジェクトが指定する 1.0.1150.38 で使えるAPIだけを使っている(`ICoreWebView2NavigationCompletedEventArgs2` は 1.0.1245.22 からなので使っていない)。
  検査に使ったのは、それより新しい版(1.0.2957.106)
- **C++はMSVCでの実ビルドと実機での動作が未確認**(MinGWでの構文・型検査のみ)。ビルドエラーが出たらその文面を教えてほしい
- `replaceApplication`(別アプリへの切り替え)は未実装で、引数検査のあと `MISC_ERR`
- ReceiverDevice、EIT検索、ストリームイベント、端末連携など STD-0011 3.1.6 以降のAPI、字幕の提示、別ストリームの選択は未実装
- セクション形式AIT(フェーズ1)、TLSのルート証明書(放送で伝送されるもの)、HTTP Basic/Digest認証のダイアログは未対応
- BMLの `getResidentAppVersion("AITControlledApp")` の応答(局のBMLが確認する可能性がある)は未対応
- 局のサーバーが MakerId/ModelId で受信機を選別している場合、既定値では弾かれる可能性がある
- 映像の位置(`videoChanged`)の座標が、DPI・拡大率の違う環境で合うか未確認
- 「任意のロケーション」を表す領域(urls が null)は、安全のためバウンダリに含めていない
