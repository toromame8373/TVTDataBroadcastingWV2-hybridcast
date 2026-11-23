import { XMLParser } from "fast-xml-parser";
import { isValidManagedUrl, type PermissionManagedArea } from "./boundary";
export { isInBoundary, resolveArea, hasFullPermission, isValidManagedUrl, type PermissionManagedArea } from "./boundary";

/**
 * XML形式AITの解析と、アプリケーションバウンダリ／権限の判定。
 * 根拠: IPTVFJ STD-0013 5.1(XML-AITの例), STD-0010 7.1.2 / 7.6, STD-0011 3.1.3
 *
 * 仕様書で未確認の点(要確認):
 *  - orgId / appId を10進数として扱っている(サンプルが "16", "1")
 *  - managedURL の包含判定を「正規化したURLの前方一致」としている
 */

export type ControlCode = "AUTOSTART" | "PRESENT" | "KILL" | "PREFETCH" | string;

export interface HybridcastApplication {
    domainName?: string;
    organizationId: number;
    applicationId: number;
    /** 例: "ISDB-HTML" */
    type: string;
    controlCode: ControlCode;
    priority: number;
    version: number;
    mhpProfile?: number;
    /** URLBase + applicationLocation を解決した起動URL */
    entryUrl: string;
    urlBase: string;
    boundary: PermissionManagedArea[];
}

const parser = new XMLParser({
    ignoreAttributes: false,
    attributeNamePrefix: "@_",
    removeNSPrefix: true, // isdb: mhp: を外す
    parseTagValue: false, // 値は文字列のまま扱う("00" や "ff" を壊さない)
    trimValues: true,
});

function asArray<T>(v: T | T[] | undefined): T[] {
    if (v === undefined) return [];
    return Array.isArray(v) ? v : [v];
}

function parseIntStrict(s: unknown, radix: number, what: string): number {
    const str = String(s ?? "").trim();
    const n = parseInt(str, radix);
    if (!/^[0-9a-fA-F]+$/.test(str) || Number.isNaN(n)) {
        throw new Error(`AIT: ${what} が数値ではありません: "${str}"`);
    }
    return n;
}

/** XML-AIT文字列を解析する。不正な項目を持つアプリは、ログを出して無視する(XML自体が壊れていれば例外)。 */
export function parseXmlAit(xml: string): HybridcastApplication[] {
    const doc = parser.parse(xml);
    const discovery = asArray<any>(doc?.ServiceDiscovery?.ApplicationDiscovery);
    const result: HybridcastApplication[] = [];
    for (const d of discovery) {
        const domainName: string | undefined = d["@_DomainName"];
        for (const app of asArray<any>(d?.ApplicationList?.Application)) {
          try {
            const id = app.applicationIdentifier;
            const desc = app.applicationDescriptor;
            const urlBase: string = app.applicationTransport?.URLBase ?? "";
            const location: string = app.applicationLocation ?? "";
            const entryUrl = new URL(location, urlBase).toString();

            const boundary: PermissionManagedArea[] = [];
            for (const bp of asArray<any>(
                app.applicationBoundaryAndPermissionDescriptor?.boundaryAndPermission,
            )) {
                const bitmaps = asArray<string>(bp.permissionBitmap).map((b) =>
                    parseIntStrict(b, 16, "permissionBitmap"));
                boundary.push({
                    permission: bitmaps.length === 0 ? null : bitmaps,
                    // managedURL は書かれたまま保持する(末尾のスラッシュの有無で、下位ドメインを含むかが変わるため)
                    urls: asArray<string>(bp.managedURL).map((u) => String(u).trim()).filter((u) => {
                        if (isValidManagedUrl(u)) return true;
                        console.error("AIT: 無効な managedURL を無視します", u);
                        return false;
                    }),
                });
            }

            result.push({
                domainName,
                organizationId: parseIntStrict(id?.orgId, 10, "orgId"),
                applicationId: parseIntStrict(id?.appId, 10, "appId"),
                type: String(desc?.type?.IsdbApp ?? ""),
                controlCode: String(desc?.controlCode ?? ""),
                priority: parseIntStrict(desc?.priority ?? "0", 16, "priority"),
                version: parseIntStrict(desc?.version ?? "0", 16, "version"),
                mhpProfile: desc?.mhpVersion?.profile !== undefined
                    ? Number(desc.mhpVersion.profile)
                    : undefined,
                entryUrl,
                urlBase,
                boundary,
            });
          } catch (e) {
            // 1件の不正(数値でない識別子など)で、他の正しいアプリまで捨てない
            console.error("AIT: 不正なアプリケーション記述を無視します", e);
          }
        }
    }
    return result;
}

/** 起動対象を選ぶ。STD-0013 5.2.3: AUTOSTART / PRESENT / KILL のみ運用。不明な制御コードは無視する。 */
export function selectLaunchTarget(apps: HybridcastApplication[]): HybridcastApplication | undefined {
    const candidates = apps.filter((a) => a.controlCode === "AUTOSTART");
    // 起動優先度は値が大きいほど優先とみなす(要確認: STD-B24 第四編 5.3.5)
    candidates.sort((a, b) => b.priority - a.priority);
    return candidates[0];
}
