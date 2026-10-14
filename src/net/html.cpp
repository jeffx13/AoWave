#include "net/html.h"
#include <QRegularExpression>
#include <QJSEngine>
#include <QJSValue>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

QString take(xmlChar *text) {
    if (!text) return {};
    const QString out = QString::fromUtf8(reinterpret_cast<const char *>(text));
    xmlFree(text);
    return out;
}

struct XPathResult {
    XPathResult(const std::shared_ptr<xmlXPathContext> &context, xmlNodePtr node, const QString &xpath) {
        if (!context || !node) return;
        context->node = node;
        const QByteArray expr = xpath.toUtf8();
        m_object = xmlXPathEvalExpression(reinterpret_cast<const xmlChar *>(expr.constData()), context.get());
    }
    ~XPathResult() { if (m_object) xmlXPathFreeObject(m_object); }
    XPathResult(const XPathResult &) = delete;
    XPathResult &operator=(const XPathResult &) = delete;

    int size() const { return nodes() ? nodes()->nodeNr : 0; }
    xmlNodePtr at(int i) const { return nodes()->nodeTab[i]; }

private:
    xmlNodeSetPtr nodes() const { return m_object ? m_object->nodesetval : nullptr; }
    xmlXPathObjectPtr m_object = nullptr;
};

}

Html::Html(const QString &html) {
    LIBXML_TEST_VERSION

    const QByteArray bytes = html.toUtf8();
    xmlDocPtr doc = htmlReadMemory(bytes.constData(), bytes.size(), nullptr, "UTF-8",
                                   HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return;

    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (!context) { xmlFreeDoc(doc); return; }

    // xmlXPathFreeContext leaves the document, so the last Node frees both.
    auto owner = std::shared_ptr<xmlXPathContext>(context, [](xmlXPathContextPtr c) {
        xmlDocPtr owned = c->doc;
        xmlXPathFreeContext(c);
        xmlFreeDoc(owned);
    });
    m_root = Node(std::move(owner), xmlDocGetRootElement(doc));
}

QVector<Html::Node> Html::Node::select(const QString &xpath) const {
    const XPathResult result(m_context, m_node, xpath);
    QVector<Node> nodes;
    nodes.reserve(result.size());
    for (int i = 0; i < result.size(); ++i)
        nodes.emplaceBack(m_context, result.at(i));
    return nodes;
}

Html::Node Html::Node::selectFirst(const QString &xpath) const {
    const XPathResult result(m_context, m_node, xpath);
    return result.size() > 0 ? Node(m_context, result.at(0)) : Node();
}

QString Html::Node::text() const {
    return m_node ? take(xmlNodeGetContent(m_node)) : QString();
}

QString Html::Node::attr(const QString &name) const {
    if (!m_node) return {};
    const QByteArray utf8 = name.toUtf8();
    return take(xmlGetProp(m_node, reinterpret_cast<const xmlChar *>(utf8.constData())));
}

QString Js::unpack(const QString &html) {
    // \s* not \n: a CRLF response puts \r between the ) and the next token.
    static QRegularExpression re{R"((\(function\(p,a,c,k,e,d\).*?\))\s*<\/script>)"};
    QRegularExpressionMatch packedMatch = re.match(html);
    if (!packedMatch.hasMatch()) return {};
    QString packed = packedMatch.captured(1);

    static const QString kJs = QStringLiteral(
        "function unPack(c){function i(a){try{var t=0,o=-1,d='';for(var e=0;e<a.length;e++){if(a[e].indexOf('{')!=-1)t++;if(a[e].indexOf('}')!=-1)t--;if(o!=t){o=t;d='';while(o>0){d+='\\t';o--;}o=t;}a[e]=d+a[e];}}finally{t=null;o=null;d=null;}return a;}var e={eval:function(x){c=x;},window:{},document:{}};eval('with(e){'+c+'}');c=(c+'').replace(/;/g,';\\n').replace(/{/g,'\\n{\\n').replace(/}/g,'\\n}\\n').replace(/\\n;\\n/g,';\\n').replace(/\\n\\n/g,'\\n');c=c.split('\\n');c=i(c);c=c.join('\\n');return c;}"
    );
    thread_local QJSEngine tEngine;
    thread_local QJSValue tFn;
    thread_local bool tInitialized = false;
    if (!tInitialized) {
        tEngine.installExtensions(QJSEngine::ConsoleExtension);
        tFn = tEngine.evaluate(kJs + QStringLiteral("\nunPack"));
        tInitialized = tFn.isCallable();
    }
    if (!tFn.isCallable()) return {};
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    QJSEngine *engine = &tEngine;
    std::thread watchdog([&, engine] {
        std::unique_lock lock(mutex);
        if (!finished.wait_for(lock, std::chrono::seconds(5), [&] { return done; })) engine->setInterrupted(true);
    });
    const QJSValue result = tFn.call(QJSValueList{tEngine.toScriptValue(packed)});
    {
        std::lock_guard lock(mutex);
        done = true;
    }
    finished.notify_one();
    watchdog.join();
    tEngine.setInterrupted(false);
    if (result.isError()) return {};
    return result.toString();
}
