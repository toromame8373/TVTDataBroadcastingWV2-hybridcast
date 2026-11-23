import { parseXmlAit, selectLaunchTarget, type HybridcastApplication } from "./ait";
import { isInBoundary } from "./boundary";
import type { AppInit } from "./hybridcast_api";

export interface HybridcastLaunchMessage {
    type: "hybridcastLaunch";
    entryUrl: string;
    /**
     * アプリが文書遷移してよいURL(アプリケーションバウンダリ。前方一致)。managedURL と起動URLのオリジン。
     * 「任意のロケーション」を表す領域(urls が null)は、安全のためバウンダリに含めない。
     */
    boundaryUrls: string[];
    init: AppInit;
}

export function buildLaunchMessage(target: HybridcastApplication): HybridcastLaunchMessage {
    const allow = new Set<string>();
    for (const b of target.boundary) {
        for (const u of b.urls) allow.add(u);
    }
    // AITの起動URLがバウンダリに含まれていない場合でも、起動したアプリがすぐ終了しないよう、
    // 起動URLのオリジンだけを厳格に(末尾のスラッシュつきで)許可する。
    if (!isInBoundary(target.boundary, target.entryUrl)) {
        console.warn("hybridcast: 起動URLがバウンダリに含まれていません", target.entryUrl);
        allow.add(new URL(target.entryUrl).origin + "/");
    }
    return {
        type: "hybridcastLaunch",
        entryUrl: target.entryUrl,
        boundaryUrls: [...allow],
        init: {
            type: target.type,
            organizationId: target.organizationId,
            applicationId: target.applicationId,
            controlCode: target.controlCode,
            autostartPriority: target.priority,
            // urls が空配列(=任意のロケーション)は null で表す(STD-0011 3.1.3.1)
            boundary: target.boundary.map((b) => ({
                permission: b.permission,
                urls: b.urls.length === 0 ? null : b.urls,
            })),
        },
    };
}

/** 内部プロキシ経由でAIT(XML)を取得して解析する。通信が無効な場合や失敗時は例外を投げる。 */
export async function fetchAit(aitUrl: string): Promise<HybridcastApplication[]> {
    const res = await window.fetch("https://tvtdatabroadcastingwv2-api.invalid/api/get/" + aitUrl, { method: "GET" });
    if (res.status !== 200) {
        throw new Error(`AITの取得に失敗 (${res.status}): ${aitUrl}`);
    }
    return parseXmlAit(await res.text());
}

/**
 * AITのURLを取得し、起動すべきHTMLアプリがあればホスト(C++)へ起動を依頼する(試験用のini指定から使う)。
 * 戻り値は起動を依頼したかどうか。
 */
export async function launchHybridcastFromAit(
    aitUrl: string,
    postToHost: (msg: HybridcastLaunchMessage) => void,
): Promise<boolean> {
    const apps = await fetchAit(aitUrl);
    const target = selectLaunchTarget(apps);
    if (!target) {
        console.warn("hybridcast: 起動対象(AUTOSTART)のアプリがありません");
        return false;
    }
    postToHost(buildLaunchMessage(target));
    return true;
}
