#pragma once
//
// PenChromeCrawler — OS의 실제 Chrome 브라우저를 CDP(Chrome DevTools Protocol)로 조종.
//
// 왜:
//   임베디드 QWebEngine은 navigator.webdriver, 자동화 시그널 등으로 봇 탐지 가능.
//   진짜 Chrome은 사용자의 쿠키/로그인/익스텐션 그대로 사용 → 사람으로 인식.
//
// 어떻게:
//   1) Chrome을 --remote-debugging-port=<port> --user-data-dir=<dir> 옵션으로 launch
//   2) http://localhost:<port>/json/version → WebSocket URL 얻기
//   3) WebSocket으로 연결 → CDP 명령 송수신 (Page.navigate, Runtime.evaluate, etc.)
//
// 사용 예:
//   PenChromeCrawler *c = new PenChromeCrawler(backend);
//   c->setUseUserProfile(true);  // 로그인된 기본 프로필 그대로 (쿠키 공유)
//   c->start([this](bool ok){
//       if (!ok) return;
//       c->navigate("https://x.com/jack", [this](bool){
//           c->getRenderedHtml([this](const QString &html){
//               // ... 저장
//           });
//       });
//   });

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QQueue>
#include <QPointer>
#include <functional>

class PenBackend;
class QProcess;
class QWebSocket;
class QNetworkAccessManager;

class PenChromeCrawler : public QObject
{
    Q_OBJECT

public:
    explicit PenChromeCrawler(PenBackend *backend, QObject *parent = nullptr);
    ~PenChromeCrawler() override;

    // 공유 프로필 사용 (사용자의 기본 Chrome 로그인 세션 그대로)
    // false면 임시 프로필 사용 (--user-data-dir=tmp).
    void setUseUserProfile(bool b) { m_useUserProfile = b; }
    bool useUserProfile() const { return m_useUserProfile; }
    // 0 이하 = 자동(기본) — Chrome 이 빈 포트를 스스로 고르게 한다(아래 m_portAuto 설명).
    void setDebugPort(int port) { m_debugPort = port > 0 ? port : 0; m_portAuto = port <= 0; }
    void setUserDataDir(const QString &d) { m_userDataDir = d; }
    // PEN 전용 영구 프로필 — 수집 캡처 Chrome(chrome_capture_profile)과 나눈다. 접두가 같아
    //   앱 시작 때의 좀비 정리 · 캐시 다듬기에는 함께 걸린다(HanishikiBackend 생성자).
    static QString defaultProfileDir();
    // PEN 프로필이 아직 없으면 예전 공용 프로필의 로그인 파일만 한 번 옮긴다. '사용자 Chrome 로그인 가져오기' 는
    //   폴더를 먼저 만들므로 그 앞에서 부른다(그 뒤에 부르면 '이미 있음' 으로 보고 건너뛴다).
    static void carryLoginFromSharedProfile(const QString &penDir, PenBackend *backend);

    // 1) Chrome 시작 + CDP 연결 (콜백 ok=true면 성공)
    void start(std::function<void(bool)> done);

    // 2) URL로 이동 + onLoad 콜백
    void navigate(const QString &url, std::function<void(bool)> done);

    // 3) JS 평가 (returnByValue=true로 결과 받음)
    void evaluate(const QString &expr, std::function<void(const QJsonValue &)> done);

    // 4) 현재 페이지의 outerHTML
    void getRenderedHtml(std::function<void(const QString &)> done);

    // 5) Network 도메인 활성화 (responseReceived 이벤트로 응답 메타데이터 받음)
    void enableNetwork(std::function<void(bool)> done);

    // 6) 특정 requestId의 응답 본문 가져오기 (Network.getResponseBody)
    void getResponseBody(const QString &requestId,
                         std::function<void(const QString &body, const QString &mimeType)> done);

    // 7) 스크롤 (window.scrollTo + JS)
    void scrollToBottom(std::function<void()> done);

    // 8) 스크린샷 (PNG 바이너리)
    void captureScreenshot(std::function<void(const QByteArray &)> done);

    // 9) Network.setCookie — 페이지 도메인에 의존하지 않고 쿠키 사전 주입
    //    cookies: { name, value, domain, path } 키를 가진 QJsonObject 배열
    void setCookies(const QJsonArray &cookies, std::function<void(bool)> done);

    // 10) Chrome 다운로드 디렉토리 설정 — 이후 모든 다운로드가 이 폴더로 떨어짐
    void setDownloadPath(const QString &path, std::function<void(bool)> done);

    // 11) 키보드 단축키 발사 — SingleFile 등 확장 트리거용
    //     modifiers: 1=Alt, 2=Ctrl, 4=Meta(Cmd), 8=Shift (조합 비트마스크)
    void dispatchKey(const QString &key, int modifiers, std::function<void()> done);

    // 종료
    void stop();
    bool isReady() const { return m_ready; }

    // 캡처된 응답을 디스크에 저장하기 시작 (start 후 enableNetwork 자동 호출)
    void setResponseSaveDir(const QString &dir) { m_responseSaveDir = dir; }
    QStringList capturedResponses() const { return m_capturedRespFiles; }

signals:
    void responseReceived(const QJsonObject &resp);   // Network.responseReceived 본문
    void networkResponseSaved(const QString &filePath); // 디스크에 저장됨
    void disconnected();

private slots:
    void onWsConnected();
    void onWsTextMessage(const QString &msg);
    void onWsError();

private:
    QString findChromeExecutable() const;
    QString resolveDebuggerWsUrl(int port) const;  // /json/version 파싱
    int sendCommand(const QString &method, const QJsonObject &params,
                    std::function<void(const QJsonValue &result, const QJsonValue &error)> cb);
    void handleEvent(const QString &method, const QJsonObject &params);

    PenBackend *m_backend = nullptr;
    QPointer<QProcess> m_chromeProc;
    QPointer<QWebSocket> m_ws;
    QNetworkAccessManager *m_nam = nullptr;

    // ★ 예전엔 9223 고정 + chrome_capture_profile 이라, 수집 캡처 Chrome(기본 9223 · 같은 프로필)과
    //   포트·프로필을 함께 썼다. 한쪽이 뜰 때 다른 쪽을 '좀비' 로 보고 끄고, 같은 프로필을 두 Chrome 이
    //   잡으려다 하나가 바로 꺼졌다. 이제 자체 프로필 모드는 --remote-debugging-port=0 으로 띄워 Chrome 이
    //   빈 포트를 고르고, 그 번호를 프로필의 DevToolsActivePort 에서 읽는다 — 어느 고정 포트와도 겹치지 않는다.
    int m_debugPort = 0;          // 0 = 아직 모름(자동). 시작한 뒤엔 실제 포트
    bool m_portAuto = true;       // setDebugPort 로 정하지 않았으면 자동
    bool m_useUserProfile = false;
    bool m_ready = false;
    int m_nextCmdId = 1;
    QMap<int, std::function<void(const QJsonValue &, const QJsonValue &)>> m_pendingCmds;

    QString m_userDataDir;     // 임시 프로필 경로 (m_useUserProfile=false일 때)
    QString m_responseSaveDir; // Network 응답 저장 디렉토리
    QStringList m_capturedRespFiles;

    // requestId → URL/method 매핑 (Network.requestWillBeSent에서 채움)
    QMap<QString, QJsonObject> m_requestMeta;
};
