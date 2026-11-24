/**
 * ページ生成時に注入する、ハイブリッドキャストの拡張API。
 * 根拠: IPTVFJ STD-0011 3.1.2〜3.1.5, 3.2 / STD-0013 8.1.4.2.17(表 8-3 権限, 表 8-4/8-5 例外)
 *
 * ホストとのやりとりは Transport を通す。WebView2では
 *   post: window.chrome.webview.postMessage
 *   listen: window.chrome.webview.addEventListener("message", ...)
 * 起動するアプリごとの情報は init として焼き込む(アプリ起動のたびにC++側が生成する)。
 *
 * 方針: 実装できない機能は「成功したふり」をしない。
 *   - 受信機が実際に行えない操作(別ストリームの選択、字幕など)は false を返す(仕様の「失敗」)
 *   - 未実装のAPI(EIT検索など 3.1.6 以降)は定義しない(機能検出で未対応と分かる)
 *   - ただし bmlCompat と ReceiverDevice は、実在のライブラリ(NHK nhkhc)が機能検出なしに
 *     触って例外で全体が止まるため、文書内に閉じた実装を定義する(各所に実機との差を明記)
 */
import { hasFullPermission, isValidManagedUrl, type PermissionManagedArea } from "./boundary";

export interface Transport {
    post(msg: HostMessage): void;
    listen(handler: (msg: ClientMessage) => void): void;
}

export interface AppInit {
    type: string;               // 例: "ISDB-HTML"
    organizationId: number;
    applicationId: number;
    controlCode: string;
    autostartPriority: number;
    /** API上の表現: urls が null なら「その他すべてのロケーション」 */
    boundary: { permission: number[] | null; urls: string[] | null }[];
}

export type HostMessage =
    | { type: "videoChanged"; left: number; right: number; top: number; bottom: number; invisible: boolean }
    | { type: "hc.audioMute"; enable: boolean }
    | { type: "hc.destroyApplication" }
    | { type: "hc.exitFromManagedState"; uri: string }
    | { type: "hc.launchDataBroadcastingBrowser" }
    | { type: "hc.addPermissionManagedArea"; urls: string[] | null };

export type ClientMessage = { type: string };

const BROADCAST_TYPE = "video/x-iptvf-broadcast";

/** STD-0013 表 8-5 */
const ERROR_MESSAGES = {
    INVALID_PARAM_ERR: "Invalid parameter",
    TIMEOUT_ERR: "Timeout",
    NOT_FOUND_ERR: "File not found",
    NOT_AUTHORIZED_ERR: "Not authorized",
    OUT_OF_RANGE_ERR: "Index out of range",
    MISC_ERR: "Unknown error",
} as const;
type ErrorCode = keyof typeof ERROR_MESSAGES;

export function installHybridcastApi(win: any, init: AppInit, transport: Transport): void {
    const nav = win.navigator;

    /** 内部表現: urls が空配列 = その他すべてのロケーション */
    const areas: PermissionManagedArea[] = init.boundary.map((b) => ({
        permission: b.permission,
        urls: b.urls ?? [],
    }));

    function hcError(code: ErrorCode): Error {
        const e: any = new win.Error(ERROR_MESSAGES[code]);
        e.name = "Error"; // STD-0013 表 8-4: 値は "Error" 固定
        e.code = code;
        return e;
    }

    /** 呼び出した文書のURLに対する「最大の許可」があるか(表 8-3) */
    const hasFull = () => hasFullPermission(areas, win.location.href);
    function requireFull(): void {
        if (!hasFull()) throw hcError("NOT_AUTHORIZED_ERR");
    }

    function parseHttpUrl(u: unknown): string | undefined {
        if (typeof u !== "string") return undefined;
        try {
            const url = new URL(u);
            if (url.protocol !== "http:" && url.protocol !== "https:") return undefined;
            return url.toString();
        } catch {
            return undefined;
        }
    }

    // --- Application / ApplicationInformationTable ---------------------------------
    const ait = {
        getApplications() {
            requireFull();
            // 自分以外のアプリケーションオブジェクトは提供しない(未実装)
            return [application];
        },
    };

    const boundaryDescriptor = {
        getCurrentBoundary() {
            requireFull();
            // 記述子もaddの追加も無ければ要素数0の配列(STD-0011 3.1.3.3)
            return areas.map((a) => ({
                permission: a.permission === null ? null : [...a.permission],
                urls: a.urls.length === 0 ? null : [...a.urls],
            }));
        },
        addPermissionManagedArea(pma: any) {
            requireFull();
            if (pma === null || typeof pma !== "object") throw hcError("INVALID_PARAM_ERR");
            let permission: number[] | null = null;
            if (pma.permission !== null && pma.permission !== undefined) {
                if (!Array.isArray(pma.permission) ||
                    !pma.permission.every((p: unknown) => Number.isInteger(p) && (p as number) >= 0 && (p as number) <= 0xffff)) {
                    throw hcError("INVALID_PARAM_ERR");
                }
                // 要素を持たない配列は null が指定されたものとして扱う
                permission = pma.permission.length === 0 ? null : [...pma.permission];
            }
            let urls: string[] | null = null;
            if (pma.urls !== null && pma.urls !== undefined) {
                if (!Array.isArray(pma.urls)) throw hcError("INVALID_PARAM_ERR");
                urls = pma.urls.map((u: unknown) => {
                    if (typeof u !== "string" || !isValidManagedUrl(u)) throw hcError("INVALID_PARAM_ERR");
                    return u.trim(); // 書かれたまま保持する(末尾のスラッシュの有無に意味がある)
                });
            }
            areas.push({ permission, urls: urls ?? [] });
            transport.post({ type: "hc.addPermissionManagedArea", urls });
        },
    };

    const application = {
        type: init.type,
        organization_id: init.organizationId,
        application_id: init.applicationId,
        control_code: init.controlCode,
        autostart_priority: init.autostartPriority,
        replaceApplication(organizationId: unknown, applicationId: unknown, uri: unknown) {
            requireFull();
            if (!Number.isInteger(organizationId) || (organizationId as number) < 0 ||
                !Number.isInteger(applicationId) || (applicationId as number) < 0 ||
                (uri !== null && uri !== undefined && parseHttpUrl(uri) === undefined)) {
                throw hcError("INVALID_PARAM_ERR");
            }
            // 未実装: 受信機側でAITを取得して別アプリへ切り替える処理(現在は常に失敗として扱う)
            throw hcError("MISC_ERR");
        },
        destroyApplication() {
            transport.post({ type: "hc.destroyApplication" });
        },
        exitFromManagedState(uri: unknown) {
            requireFull();
            // 最大 250 文字(STD-0013 表 8-11)
            if (typeof uri !== "string" || uri.length === 0 || uri.length > 250 || parseHttpUrl(uri) === undefined) {
                throw hcError("INVALID_PARAM_ERR");
            }
            transport.post({ type: "hc.exitFromManagedState", uri: new URL(uri).toString() });
        },
        getOwnerAIT() {
            requireFull();
            return ait;
        },
        getApplicationBoundaryAndPermissionDescriptor() {
            requireFull();
            return boundaryDescriptor;
        },
    };

    const applicationManager = {
        getOwnerApplication(document?: any) {
            if (document !== undefined) {
                if (document === null || typeof document !== "object" || document.nodeType !== 9) {
                    throw hcError("INVALID_PARAM_ERR");
                }
                // このアプリの文書(最上位とそのiframe)以外は、該当するアプリケーションなし
                const owner = document.defaultView;
                if (document !== win.document && (owner == null || owner.top !== win.top)) return null;
            }
            return application;
        },
        launchDataBroadcastingBrowser(startup: any) {
            requireFull();
            if (startup === null || typeof startup !== "object") throw hcError("INVALID_PARAM_ERR");
            // 8.1.3.10.1: 放送映像・音声は遷移前の提示を継続したまま、BMLアプリケーションへ遷移する
            transport.post({ type: "hc.launchDataBroadcastingBrowser" });
        },
    };

    const capabilities = {
        hasCapability(query: unknown, ...params: unknown[]) {
            // 受信機が実際に対応している機能だけ true(表 8-14 / 表 8-59 のうち、フェーズ0に対応)
            if (query === "AITControlledAppEngineFunction") return params[0] === "IPTV-F" && params[1] === "HTML5_ph0";
            if (query === "AITTransportMethod") return params[0] === "XML" && params[1] === "HTTP";
            return false;
        },
    };

    // --- BML互換オブジェクト(navigator.bmlCompat.browserPseudo) ---------------------
    // ハイブリッドキャストのアプリは、BMLブラウザの疑似オブジェクト(browserPseudo)を
    // アプリ側の文書からも触れる前提で書かれている。実例: NHKの共通ライブラリ nhkhc は
    // 初期化時に navigator.bmlCompat.browserPseudo.Greg[53] を読み(UAに Hybridcast/ を
    // 含む場合)、bmlCompat が無いと例外でライブラリ全体が停止し、アプリが起動しない。
    // 形はBML側の web-bml の browser オブジェクトに合わせる:
    //   Greg / Ureg: 64要素の文字列配列(初期値は空文字列、添字で直接読み書き)
    //   readPersistentArray(filename, structure): 保存が無ければ null、あれば値の配列
    //   writePersistentArray(filename, structure, data, period): 成功で書き込んだバイト数、失敗で NaN
    //   structure は "U:1B,S:1V,..." 形式(web-bml の parseBinaryStructure と同じ文法)
    // 制約(実機との違い): アプリの文書はBMLの文書と別文書のため、Greg と永続配列は
    //   BML側とは共有されず、文書が破棄されれば消える(実機ではGreg・NVRAMはBMLブラウザと
    //   共有され、NVRAMは電源を切っても残る)。ホスト(C++)経由の共有は未実装で、
    //   ここでは文書内に閉じたメモリ上の実装を提供する。period(保存期限)は無視する。
    const persistentArrays = new Map<string, unknown[]>();

    /** 構造文字列をフィールド種別の列にする(web-bmlと同じ文法。不正なら null) */
    function parsePersistentStructure(structure: unknown): ("num" | "str" | "pad")[] | null {
        if (typeof structure !== "string" || structure.length === 0) return null;
        const fields: ("num" | "str" | "pad")[] = [];
        for (const part of structure.split(",")) {
            const m = /^([BUISZP]):([1-9][0-9]*)([BbVv])$/.exec(part.trim());
            if (!m) return null;
            fields.push(m[1] === "P" ? "pad" : m[1] === "S" || m[1] === "Z" ? "str" : "num");
        }
        return fields;
    }

    const browserPseudo = {
        Greg: Array.from({ length: 64 }, () => ""),
        Ureg: Array.from({ length: 64 }, () => ""),
        readPersistentArray(filename: unknown, structure: unknown): unknown[] | null {
            if (typeof filename !== "string" || !filename.startsWith("nvram://")) return null;
            const fields = parsePersistentStructure(structure);
            if (fields === null) return null;
            const stored = persistentArrays.get(filename);
            if (stored === undefined) return null;
            let i = 0;
            return fields.filter((f) => f !== "pad").map((f) => {
                const v = stored[i++];
                if (f === "num") return typeof v === "number" && Number.isFinite(v) ? v : 0;
                return typeof v === "string" ? v : "";
            });
        },
        writePersistentArray(filename: unknown, structure: unknown, data: unknown, _period?: unknown): number {
            if (typeof filename !== "string" || !filename.startsWith("nvram://")) return NaN;
            const fields = parsePersistentStructure(structure);
            if (fields === null || !Array.isArray(data)) return NaN;
            const dataFields = fields.filter((f) => f !== "pad");
            if (dataFields.length > data.length) return NaN;
            persistentArrays.set(filename, dataFields.map((f, i) => {
                const v = data[i];
                if (f === "num") {
                    const n = typeof v === "number" ? v : Number(v);
                    return Number.isFinite(n) ? n : 0;
                }
                return v === null || v === undefined ? "" : String(v);
            }));
            // 戻り値は書き込んだバイト数(web-bmlと同じ)。固定長(B)は宣言どおり、
            // 可変長(V)は文字列のUTF-8バイト数・数値は8バイトとして数える概算。
            let bytes = 0;
            let i = 0;
            for (const part of (structure as string).split(",")) {
                const m = /^([BUISZP]):([1-9][0-9]*)([BbVv])$/.exec(part.trim());
                if (!m) continue;
                if (m[3] === "B" || m[3] === "b") bytes += Number(m[2]);
                else if (m[1] !== "P") bytes += typeof data[i] === "string" ? new TextEncoder().encode(data[i] as string).length : 8;
                if (m[1] !== "P") i++;
            }
            return bytes;
        },
    };

    const bmlCompat = { browserPseudo };

    // --- 受信機デバイスオブジェクト(navigator.receiverDevice) -----------------------
    // ハイブリッドキャスト受信機に標準で存在するオブジェクト(IPTVFJ STD-0011 の
    // ReceiverDevice)。nhkhc は UA に Hybridcast/ を含む場合、初期化で無防備に触る:
    // receiverDevice が未定義だと navigator.receiverDevice.streamEvent の読み取りで
    // 例外になり、bmlCompat のときと同様にライブラリ全体が停止する(2026-11-23 実機ログ)。
    // nhkhc-2.2.0.min.js の全12箇所の利用は次のとおり(実物で列挙):
    //   streamEvent.sourceIs("onair")                 … NVRAM書込を行うかの判定(1箇所)
    //   streamEvent.addEventIDUpdateListener(fn)      … 番組情報の更新通知の登録(2箇所)
    //   streamEvent.addGeneralEventMessageListener(desc, fn) /
    //   streamEvent.removeGeneralEventMessageListener(desc, fn)
    //                                                 … 緊急情報メッセージの登録/解除(各2箇所、try/catch内)
    //   getCurrentEventInformation(fn)                … 現在の番組情報の取得(2箇所)
    //   sendTextToCompanionDevice(text)               … 端末連携への送信(1箇所、戻り値は未使用)
    //   addCompanionDeviceTextMessageListener(fn)     … 端末連携からの受信登録(1箇所)
    //   setURLForCompanionDevice(url, options)        … 端末連携へのURL提示(1箇所、戻り値は未使用)
    // なお NHK アプリ本体(p/index.js)は getSystemInformation(null) も呼ぶ
    // (makerid/modelname/browsername/browserversion を読む。FIX2 時点では未提供だった)。
    // 実装方針(bmlCompat と同じ、文書内に閉じた実装):
    //   - sourceIs("onair") は true。このプラグインではアプリは選局中の放送から起動し、
    //     選局が変わればアプリは終了するので、アプリの生存中は常に対象の放送を受信している。
    //     それ以外のソースは false(受信していないものを主張しない)。true を返すことで、
    //     nhkhc の NVRAM 書き込みは browserPseudo 側へ実際に行われ、読み戻しと一致する
    //     (false だと「書き込まずに成功扱い」になり、後段の検証と食い違う)。
    //   - 番組情報(EIT)をホストから文書へ届ける経路は無いため、getCurrentEventInformation は
    //     コールバックを undefined で呼ぶ。nhkhc は undefined を「情報なし」として扱い、
    //     自前の初期値で初期化を続行する設計なので、これで初期化は完了する。番組情報は
    //     捏造しない。addEventIDUpdateListener のリスナーも発火しない(nhkhc には数秒後の
    //     フォールバック経路があり、そちらでも初期化は完了する)。
    //   - 一般イベントメッセージ(緊急情報)もホストから届かないため、登録だけ受けて発火しない。
    //     setEmergency の登録自体は成功するが、緊急情報の通知はアプリに届かない(実機との差)。
    //   - 端末連携(コンパニオンデバイス)はホスト未対応のため、送信はどこにも届かず、
    //     受信リスナーも発火しない(実機で連携機器が存在しない場合に相当する振る舞い)。
    const eventIDUpdateListeners = new Set<unknown>();
    const generalEventMessageListeners = new Set<{ messageGroupId: unknown; messageId: unknown; listener: unknown }>();
    const companionTextMessageListeners = new Set<unknown>();

    const streamEvent = {
        sourceIs(source: unknown): boolean {
            return source === "onair";
        },
        addEventIDUpdateListener(listener: unknown): void {
            if (typeof listener === "function") eventIDUpdateListeners.add(listener);
        },
        // nhkhc は使わないが、add と対になる解除も用意する
        removeEventIDUpdateListener(listener: unknown): void {
            eventIDUpdateListeners.delete(listener);
        },
        addGeneralEventMessageListener(descriptor: any, listener: unknown): void {
            if (typeof listener !== "function") return;
            generalEventMessageListeners.add({
                messageGroupId: descriptor?.message_group_id,
                messageId: descriptor?.message_id,
                listener,
            });
        },
        removeGeneralEventMessageListener(descriptor: any, listener: unknown): void {
            for (const entry of generalEventMessageListeners) {
                if (entry.listener === listener &&
                    entry.messageGroupId === descriptor?.message_group_id &&
                    entry.messageId === descriptor?.message_id) {
                    generalEventMessageListeners.delete(entry);
                }
            }
        },
    };

    const receiverDevice = {
        streamEvent,
        getSystemInformation(_arg?: unknown): { makerid: string; modelname: string; browsername: string; browserversion: string } {
            // NHK アプリ本体(p/index.js)が初期化で呼ぶ。実機では端末の製造元・機種名
            // などが返るが、このプラグインに実機情報は無いため中立な値を返す。
            // makerid を "" にすることで、SHARP("11")・SONY("12")専用の
            // tv-sc.nhk.jp への SSL 可否確認 POST は走らない(アプリ側の分岐による)。
            return {
                makerid: "",
                modelname: "TVTDataBroadcastingWV2",
                browsername: "WebView2",
                browserversion: "",
            };
        },
        getCurrentEventInformation(callback: unknown): void {
            if (typeof callback !== "function") return;
            // 番組情報は提供できないため、undefined(情報なし)で応答する
            win.setTimeout(() => (callback as (info: undefined) => void)(undefined), 0);
        },
        sendTextToCompanionDevice(_text: unknown): void {
            // 連携機器は存在しないため、送信はどこにも届かない
        },
        addCompanionDeviceTextMessageListener(listener: unknown): void {
            if (typeof listener === "function") companionTextMessageListeners.add(listener);
        },
        setURLForCompanionDevice(_url: unknown, _options?: unknown): void {
            // 連携機器は存在しないため、提示先がない
        },
    };

    define(nav, "applicationManager", applicationManager);
    define(nav, "capabilities", capabilities);
    define(nav, "bmlCompat", bmlCompat);
    define(nav, "receiverDevice", receiverDevice);

    // --- 放送映像音声オブジェクト --------------------------------------------------
    function augment(el: any) {
        if (el.__hcAugmented) return;
        if ((el.getAttribute("type") ?? "").toLowerCase() !== BROADCAST_TYPE) return;
        // 放送映像音声オブジェクトの有効化には権限が要る(表 8-3)。無権限なら通常のobject要素のまま。
        if (!hasFull()) return;
        el.__hcAugmented = true;

        const state = {
            fullscreen: false,
            mute: false,
            videoSrc: DEFAULT_STREAM,
            audioSrc: DEFAULT_STREAM,
        };
        let initialized = false;

        // 放送映像の表示位置をホストへ伝える(既存の videoChanged メッセージをそのまま使う)。
        // 座標はビューポートのCSSピクセル(BML側と同じ扱いと仮定。DPI/ズーム時の挙動は未検証)。
        const reportRect = () => {
            const r = state.fullscreen
                ? { left: 0, top: 0, right: win.innerWidth, bottom: win.innerHeight }
                : el.getBoundingClientRect();
            transport.post({ type: "videoChanged", left: r.left, right: r.right, top: r.top, bottom: r.bottom, invisible: false });
        };

        /** param要素による初期設定(STD-0011 表 3-4)。オブジェクト生成後に1回だけ反映する。 */
        const initFromParams = () => {
            if (initialized) return;
            initialized = true;
            const p: Record<string, string> = {};
            el.querySelectorAll("param").forEach((n: Element) => {
                const name = n.getAttribute("name");
                const value = n.getAttribute("value");
                if (name !== null && value !== null && !(name in p)) p[name] = value;
            });
            if (p["video_src"] !== undefined && isDefaultStream(p["video_src"])) state.videoSrc = p["video_src"];
            if (p["audio_src"] !== undefined && isDefaultStream(p["audio_src"])) state.audioSrc = p["audio_src"];
            if (p["audio_mute"] === "enable") {
                state.mute = true;
                transport.post({ type: "hc.audioMute", enable: true });
            }
            if (p["fullscreen"] === "enable") state.fullscreen = true;
            reportRect();
        };
        const scheduleInit = () => {
            if (win.document.readyState === "loading") {
                win.document.addEventListener("DOMContentLoaded", initFromParams, { once: true });
            } else {
                win.setTimeout(initFromParams, 0);
            }
        };
        scheduleInit();

        if (typeof win.ResizeObserver === "function") new win.ResizeObserver(reportRect).observe(el);
        win.addEventListener("resize", reportRect);
        win.addEventListener("scroll", reportRect, { passive: true });

        const methods: Record<string, (...a: any[]) => any> = {
            enableFullscreen() { initFromParams(); state.fullscreen = true; reportRect(); return true; },
            disableFullscreen() { initFromParams(); state.fullscreen = false; reportRect(); return true; },
            isFullscreen() { initFromParams(); return state.fullscreen; },
            enableAudioMute() { initFromParams(); state.mute = true; transport.post({ type: "hc.audioMute", enable: true }); return true; },
            disableAudioMute() { initFromParams(); state.mute = false; transport.post({ type: "hc.audioMute", enable: false }); return true; },
            isAudioMute() { initFromParams(); return state.mute; },
            // 受信機が選べるのは現在のサービスの既定のストリームだけ。それ以外の指定は失敗(false)。
            setVideoSrc(url: unknown) {
                initFromParams();
                if (!isDefaultStream(url)) return false;
                state.videoSrc = url as string;
                return true;
            },
            getVideoSrc() { initFromParams(); return state.videoSrc; },
            setAudioSrc(url: unknown) {
                initFromParams();
                if (!isDefaultStream(url)) return false;
                state.audioSrc = url as string;
                return true;
            },
            getAudioSrc() { initFromParams(); return state.audioSrc; },
            // 字幕の提示と本文取得はホスト側に未実装のため、すべて失敗/存在しない扱い。
            setCaptionSrc(_url: unknown) { return false; },
            getCaptionComponentURL() { return null; },
            isCaptionExistent(_url: unknown) { return false; },
            setCaptionVisibility(_flag?: boolean) { return false; },
            isCaptionVisible() { return false; },
            addCaptionListener(_listener: unknown, _url?: string) { /* 字幕本文は届かない(未実装) */ },
            removeCaptionListener(_listener?: unknown) { /* 同上 */ },
        };
        for (const [name, fn] of Object.entries(methods)) {
            define(el, name, fn);
        }
    }

    // 放送映像音声オブジェクトの場所は、放送映像を通す「透明な穴」にする。
    // 何もしないと、Chromium が未対応のプラグインの代替表示(「このプラグインはサポートされていません」)で
    // その場所を覆ってしまう。visibility: hidden なら、配置(getBoundingClientRect)はそのままで、表示だけが消える。
    // 権限のない文書では、放送映像音声オブジェクトを有効にしないので、何もしない。
    if (hasFull()) injectVideoHoleStyle(win);

    const scan = (root: ParentNode) => root.querySelectorAll("object").forEach(augment);
    new win.MutationObserver((records: MutationRecord[]) => {
        for (const r of records) {
            r.addedNodes.forEach((n: any) => {
                if (n.nodeType !== 1) return;
                if (n.tagName === "OBJECT") augment(n);
                else scan(n);
            });
            // type 属性が後から付く場合
            if (r.type === "attributes" && (r.target as any).tagName === "OBJECT") augment(r.target);
        }
    }).observe(win.document, { childList: true, subtree: true, attributes: true, attributeFilter: ["type"] });
    scan(win.document);
}

const VIDEO_HOLE_CSS = `object[type="${BROADCAST_TYPE}" i]{visibility:hidden !important;}`;

/**
 * 放送映像音声オブジェクトを、非表示(配置は維持)にするスタイルを文書へ加える。
 * ページ生成時はまだ head が無いので、まず constructable stylesheet(adoptedStyleSheets)を使い、
 * 使えない環境では style 要素を文書ができた時点で加える。
 */
export function injectVideoHoleStyle(win: any): void {
    try {
        const sheet = new win.CSSStyleSheet();
        sheet.replaceSync(VIDEO_HOLE_CSS);
        win.document.adoptedStyleSheets = [...win.document.adoptedStyleSheets, sheet];
        return;
    } catch {
        // 続けて、style 要素で試す
    }
    const add = () => {
        const style = win.document.createElement("style");
        style.setAttribute("data-hybridcast", "video-hole");
        style.textContent = VIDEO_HOLE_CSS;
        (win.document.head ?? win.document.documentElement).appendChild(style);
    };
    if (win.document.documentElement) add();
    else win.document.addEventListener("DOMContentLoaded", add, { once: true });
}

/** 現在のサービスの既定のストリーム(STD-0011 3.2.2 の arib://-1.-1.-1/-1) */
const DEFAULT_STREAM = "arib://-1.-1.-1/-1";

/** 受信機が選べるのは既定のストリームだけ(チャンネルIDや番組指定を伴うものは選べない) */
export function isDefaultStream(url: unknown): boolean {
    return url === DEFAULT_STREAM;
}

/** arib://<onid>.<tsid>.<sid>[;<content_id>][.<event_id>]/<component_tag>[;<channel_id>][;<language_tag>] (STD-0011 3.2.2) */
export function isAribUrl(url: unknown): boolean {
    return typeof url === "string" && /^arib:\/\/-?\d+\.-?\d+\.-?\d+(;\d+)?(\.\d+)?\/-?\d+(;\d+)?(;[A-Za-z0-9_-]+)?$/.test(url);
}

function define(target: any, name: string, value: unknown) {
    Object.defineProperty(target, name, { value, configurable: true, enumerable: false, writable: false });
}
