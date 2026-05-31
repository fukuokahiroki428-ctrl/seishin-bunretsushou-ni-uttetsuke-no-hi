#pragma once
//
// RealChromeCrawler — OS의 실제 Chrome 브라우저를 CDP(Chrome DevTools Protocol)로 조종.
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
//   RealChromeCrawler *c = new RealChromeCrawler(backend);
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

class RealChromeCrawler : public QObject
{
    Q_OBJECT

public:
    explicit RealChromeCrawler(PenBackend *backend, QObject *parent = nullptr);
    ~RealChromeCrawler() override;

    // 공유 프로필 사용 (사용자의 기본 Chrome 로그인 세션 그대로)
    // false면 임시 프로필 사용 (--user-data-dir=tmp).
    void setUseUserProfile(bool b) { m_useUserProfile = b; }
    bool useUserProfile() const { return m_useUserProfile; }
    void setDebugPort(int port) { m_debugPort = port; }
    void setUserDataDir(const QString &d) { m_userDataDir = d; }

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

    int m_debugPort = 9223;
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
