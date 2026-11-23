/**
 * アプリケーションバウンダリとアクセス権限の判定 (依存なし。注入スクリプトにも含める)。
 * 根拠: IPTVFJ STD-0010 7.6 / STD-0013 6.1.3.1(表 6-1), 6.1.3(最長一致), 8.1.4.2.17.1(表 8-3)
 *
 * 表 6-1 の規則:
 *  - スキーマは http / https。一致が必要。
 *  - ドメインだけを書いた場合(例 "http://nhk.or.jp")は、下位のドメイン(hc.nhk.or.jp など)も含む。
 *  - パスを書いた場合は、ドメインを厳格に解釈し、下位のドメインを含まない。
 *    ドメイン直後のスラッシュ(例 "http://nhk.or.jp/")だけの記述も、パスの指定に当たる。
 *  - パスは、指定したパスより下位のディレクトリを含む。
 *  - 権限は、URLに最長一致(最も階層の深い)バウンダリのものが適用される。記述順にはよらない。
 *
 * 仕様で未確認の点:
 *  - ポート番号の扱い(ここでは、既定のポートを除き、ポート番号も一致を要求する)
 *  - permission 配列の各要素の意味(STD-B24 第四編)。「null、または全要素が 0x1fff」を最大の許可とみなす。
 */

export interface PermissionManagedArea {
    /** 権限ビットマップの配列。null は「最大の許可」 */
    permission: number[] | null;
    /** managedURL の記述(書かれたまま。末尾のスラッシュの有無に意味がある)。空配列は「その他すべてのロケーション」 */
    urls: string[];
}

/** 運用規定で使う、全API許可のビットマップ値 (STD-0013 8.1.4.2.17.1) */
export const FULL_PERMISSION = 0x1fff;

interface ParsedUrl {
    scheme: string; // "http:" | "https:"
    host: string;   // 小文字
    port: string;   // 既定のポートは ""
    path: string;   // "/" 始まり(パス指定の無い managedURL では "")
    hasPath: boolean;
}

function parse(raw: string, isManaged: boolean): ParsedUrl | undefined {
    if (typeof raw !== "string") return undefined;
    const text = raw.trim();
    let url: URL;
    try {
        url = new URL(text);
    } catch {
        return undefined;
    }
    if (url.protocol !== "http:" && url.protocol !== "https:") return undefined;
    // "http://nhk.or.jp@evil.example/" のようにユーザー情報で偽装した文書は、どのバウンダリにも含めない
    if (url.username !== "" || url.password !== "") return undefined;
    if (url.hostname === "" || url.hostname.includes("[")) return undefined;
    if (isManaged && (url.search !== "" || url.hash !== "" || /[?#]/.test(text))) return undefined;
    // managedURL は「ドメイン直後にスラッシュがあるか」に意味がある。正規化前の文字列で判定する。
    const hasPath = isManaged ? /^https?:\/\/[^/?#]*\//i.test(text) : true;
    return {
        scheme: url.protocol,
        host: url.hostname.toLowerCase(),
        port: url.port,
        path: hasPath ? url.pathname : "",
        hasPath,
    };
}

/** managedURL の記述として有効か(http/https の絶対URLで、クエリ・フラグメント・ユーザー情報を含まない) */
export function isValidManagedUrl(raw: string): boolean {
    return parse(raw, true) !== undefined;
}

function pathContains(managedPath: string, docPath: string): boolean {
    // ディレクトリ境界を守る: "/sub" は "/sub" と "/sub/..." に一致し、"/subversion" には一致しない
    const base = managedPath.endsWith("/") ? managedPath : managedPath + "/";
    return docPath === managedPath || docPath.startsWith(base);
}

function matches(m: ParsedUrl, doc: ParsedUrl): boolean {
    if (m.scheme !== doc.scheme) return false;
    if (m.port !== doc.port) return false;
    if (m.hasPath) {
        // パスを書いた場合は、ドメインを厳格に解釈する
        if (m.host !== doc.host) return false;
        return pathContains(m.path, doc.path);
    }
    // ドメインだけの場合は、下位のドメインを含む("." を挟んだ後方一致。"xnhk.or.jp" は含まない)
    return doc.host === m.host || doc.host.endsWith("." + m.host);
}

/** 狭さ(最長一致)。パスが長いほど狭く、同じなら、ドメインの階層が深いほど狭い。パス指定なしは -1。 */
function specificity(m: ParsedUrl): [number, number] {
    return [m.hasPath ? m.path.length : -1, m.host.split(".").length];
}

function compareSpecificity(a: [number, number], b: [number, number]): number {
    return a[0] !== b[0] ? a[0] - b[0] : a[1] - b[1];
}

/**
 * その文書URLに最長一致するバウンダリ領域を返す(STD-0013 6.1.3)。
 * 「その他すべてのロケーション」(urls が空)は最も広い扱いで、他に一致する領域があればそちらが優先される。
 * どの領域にも含まれなければ undefined。
 */
export function resolveArea(
    boundary: PermissionManagedArea[],
    rawUrl: string,
): PermissionManagedArea | undefined {
    const doc = parse(rawUrl, false);
    if (!doc) return undefined;
    let best: PermissionManagedArea | undefined;
    let bestScore: [number, number] = [-2, 0];
    for (const area of boundary) {
        if (area.urls.length === 0) {
            if (compareSpecificity([-1.5, 0], bestScore) >= 0) {
                best = area;
                bestScore = [-1.5, 0];
            }
            continue;
        }
        for (const raw of area.urls) {
            const m = parse(raw, true);
            if (!m || !matches(m, doc)) continue;
            const score = specificity(m);
            if (compareSpecificity(score, bestScore) >= 0) { // 同じ狭さなら後の記述を採用
                best = area;
                bestScore = score;
            }
        }
    }
    return best;
}

/** 文書遷移が許されるか。「その他すべてのロケーション」は、安全のためバウンダリに含めない(権限のみに影響)。 */
export function isInBoundary(boundary: PermissionManagedArea[], url: string): boolean {
    const area = resolveArea(boundary, url);
    return area !== undefined && area.urls.length > 0;
}

/** 最大の許可(拡張APIの全使用許可)があるか。表 8-3: 0x1fff 以外は 0 として扱う。 */
export function hasFullPermission(boundary: PermissionManagedArea[], url: string): boolean {
    const area = resolveArea(boundary, url);
    if (!area) return false;
    if (area.permission === null) return true;
    return area.permission.length > 0 && area.permission.every((p) => p === FULL_PERMISSION);
}
