import type { HybridcastApplication } from "./ait";
import { buildLaunchMessage, fetchAit, type HybridcastLaunchMessage } from "./launch";

/**
 * BMLの拡張関数 startReceivingAIT / getReceivedAIT / startAITControlledApp の実体
 * (IPTVFJ STD-0013 6.1.5, 8.1.3.4。関数自体は ARIB STD-B24 第二編 7.6.20 で定義されるが、この資料は未入手)。
 *
 * web-bml はこれらの関数を持たないため、patches/web-bml の修正が globalThis.__bmlAitControlledApp を呼ぶ。
 * 戻り値の約束(STD-0013 6.1.5 の記述例より):
 *   startReceivingAIT   : 1(受付) / NaN(失敗)
 *   getReceivedAIT      : -1(失敗) / 0(取得中) / Array(取得済み。Array[1][0]=組織識別, Array[1][1]=アプリケーション識別)
 *   startAITControlledApp: 1(受付) / NaN(失敗)。引数は全て必須(STD-0013 8.1.3.4)
 *
 * 未確認: Array[0] の意味(ここでは件数を入れている)、Array[1]以降の追加要素。
 */

type Entry =
    | { state: "pending" }
    | { state: "error" }
    | { state: "done"; apps: HybridcastApplication[] };

/** 1つの文書で保持するAITは最大10回分(STD-0013 6.1.5) */
const MAX_ENTRIES = 10;

export interface BmlAitControlledApp {
    startReceivingAIT(url: string): number;
    getReceivedAIT(url: string): number | unknown[];
    startAITControlledApp(organizationId: number, applicationId: number, url: string): number;
}

const isHttp = (u: string) => /^https?:\/\//i.test(u);

export type LogFn = (level: "info" | "warn" | "error", text: string) => void;

const defaultLog: LogFn = (level, text) => console[level === "info" ? "log" : level]("hybridcast:", text);

export function createBmlAitBridge(
    postToHost: (msg: HybridcastLaunchMessage) => void,
    fetchApps: (url: string) => Promise<HybridcastApplication[]> = fetchAit,
    log: LogFn = defaultLog,
): BmlAitControlledApp {
    const cache = new Map<string, Entry>();

    function receive(url: string): Promise<HybridcastApplication[]> {
        cache.delete(url); // 挿入順を新しくする
        cache.set(url, { state: "pending" });
        while (cache.size > MAX_ENTRIES) {
            cache.delete(cache.keys().next().value as string);
        }
        const p = fetchApps(url);
        p.then(
            (apps) => { if (cache.has(url)) cache.set(url, { state: "done", apps }); },
            (e) => {
                log("error", `AITの取得に失敗しました: ${url} (${e instanceof Error ? e.message : String(e)})`);
                if (cache.has(url)) cache.set(url, { state: "error" });
            },
        );
        return p;
    }

    /**
     * BMLが渡す組織識別・アプリケーション識別から、AIT内のアプリを探す。
     * 実際のNHKのBMLは、AITに org=16 と書かれたアプリを startAITControlledApp(10, 1, ...) で呼ぶ。
     * 0x10 = 16 なので、BMLは16進の桁をそのまま数値として渡していると考えられる
     * (ARIB STD-B24 第二編 7.6.20 は未入手。実際の呼び出しからの推測)。
     * 誤って別のアプリを起動しないよう、10進として完全一致するものを先に探し、無ければ16進として読み替える。
     */
    function findApp(apps: HybridcastApplication[], org: number, app: number): HybridcastApplication | undefined {
        const exact = apps.find((a) => a.organizationId === org && a.applicationId === app);
        if (exact) return exact;
        const hexDigits = (n: number) => (/^[0-9]+$/.test(String(n)) ? parseInt(String(n), 16) : NaN);
        const hexOrg = hexDigits(org);
        const hexApp = hexDigits(app);
        return apps.find((a) => a.organizationId === hexOrg && a.applicationId === hexApp);
    }

    function launch(apps: HybridcastApplication[], org: number, app: number): boolean {
        const target = findApp(apps, org, app);
        if (!target || target.controlCode === "KILL") {
            const known = apps.map((a) => `${a.organizationId}/${a.applicationId}(${a.controlCode})`).join(", ");
            log("error", `AITに起動できる該当アプリがありません: 組織識別=${org} アプリ識別=${app} (AITの内容: ${known || "なし"})`);
            return false;
        }
        log("info", `AITのアプリを起動します: 組織識別=${target.organizationId} アプリ識別=${target.applicationId} ${target.entryUrl}`);
        postToHost(buildLaunchMessage(target));
        return true;
    }

    return {
        startReceivingAIT(url) {
            if (typeof url !== "string" || !isHttp(url)) {
                log("warn", `startReceivingAIT: URLが不正です: ${String(url)}`);
                return NaN;
            }
            log("info", `startReceivingAIT: ${url}`);
            receive(url).catch(() => {});
            return 1;
        },
        getReceivedAIT(url) {
            const e = cache.get(url);
            if (!e || e.state === "error" || (e.state === "done" && e.apps.length === 0)) return -1;
            if (e.state === "pending") return 0;
            return [e.apps.length, ...e.apps.map((a) => [a.organizationId, a.applicationId])];
        },
        startAITControlledApp(organizationId, applicationId, url) {
            // 引数は全て必須。省略・不正はアプリケーションエンジンを起動せず NaN
            if (!Number.isInteger(organizationId) || !Number.isInteger(applicationId) ||
                typeof url !== "string" || !isHttp(url)) {
                log("warn", `startAITControlledApp: 引数が不正です (${String(organizationId)}, ${String(applicationId)}, ${String(url)})`);
                return NaN;
            }
            log("info", `startAITControlledApp: 組織識別=${organizationId} アプリ識別=${applicationId} ${url}`);
            const cached = cache.get(url);
            if (cached?.state === "done") {
                return launch(cached.apps, organizationId, applicationId) ? 1 : NaN;
            }
            // 未取得ならBMLブラウザが取得してから起動する(STD-0013 8.1.3.4)。結果は非同期のため受付だけ返す
            receive(url).then((apps) => launch(apps, organizationId, applicationId)).catch(() => {});
            return 1;
        },
    };
}

export function installBmlAitBridge(postToHost: (msg: HybridcastLaunchMessage) => void, log: LogFn = defaultLog): void {
    (globalThis as any).__bmlAitControlledApp = createBmlAitBridge(postToHost, fetchAit, log);
}
