/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/


#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

#include <QtQuick/QQuickWindow>
#include <private/qquickwindow_p.h>

#include <QtQuick/private/qsgrenderer_p.h>

#include "qsgthreadedrenderloop_p.h"

/*
   Overall design:

   There are two classes here. QSGThreadedRenderLoop and
   QSGRenderThread. All communication between the two is based on
   event passing and we have a number of custom events.

   In this implementation, the render thread is never blocked and the
   GUI thread will initiate a polishAndSync which will block and wait
   for the render thread to pick it up and release the block only
   after the render thread is done syncing. The reason for this
   is:

   1. Clear blocking paradigm. We only have one real "block" point
   (polishAndSync()) and all blocking is initiated by GUI and picked
   up by Render at specific times based on events. This makes the
   execution deterministic.

   2. Render does not have to interact with GUI. This is done so that
   the render thread can run its own animation system which stays
   alive even when the GUI thread is blocked doing i/o, object
   instantiation, QPainter-painting or any other non-trivial task.

   ---

   The render loop is active while any window is exposed. All visible
   windows are tracked, but only exposed windows are actually added to
   the render thread and rendered. That means that if all windows are
   obscured, we might end up cleaning up the SG and GL context (if all
   windows have disabled persistency). Especially for multiprocess,
   low-end systems, this should be quite important.

 */

QT_BEGIN_NAMESPACE


// #define QSG_RENDER_LOOP_DEBUG
// #define QSG_RENDER_LOOP_DEBUG_FULL
#ifdef QSG_RENDER_LOOP_DEBUG
#define QSG_RENDER_LOOP_DEBUG_BASIC
#endif

#ifdef QSG_RENDER_LOOP_DEBUG_FULL
#define QSG_RENDER_LOOP_DEBUG_BASIC
#endif

#if defined (QSG_RENDER_LOOP_DEBUG_FULL)
#  define RLDEBUG1(x) qDebug("%s : %4d - %s", __FILE__, __LINE__, x);
#  define RLDEBUG(x) qDebug("%s : %4d - %s", __FILE__, __LINE__, x);
#elif defined (QSG_RENDER_LOOP_DEBUG_BASIC)
#  define RLDEBUG1(x) qDebug("%s : %4d - %s", __FILE__, __LINE__, x);
#  define RLDEBUG(x)
#else
#  define RLDEBUG1(x)
#  define RLDEBUG(x)
#endif


static int get_env_int(const char *name, int defaultValue)
{
    QByteArray content = qgetenv(name);

    bool ok = false;
    int value = content.toInt(&ok);
    return ok ? value : defaultValue;
}


static inline int qsgrl_animation_interval() {
    qreal refreshRate = QGuiApplication::primaryScreen()->refreshRate();
    // To work around that some platforms wrongfully return 0 or something
    // bogus for refreshrate
    if (refreshRate < 1)
        return 16;
    return int(1000 / refreshRate);
}


#ifndef QSG_NO_WINDOW_TIMING
static bool qquick_window_timing = !qgetenv("QML_WINDOW_TIMING").isEmpty();
static QTime threadTimer;
static int syncTime;
static int renderTime;
static int sinceLastTime;
#endif

extern Q_GUI_EXPORT QImage qt_gl_read_framebuffer(const QSize &size, bool alpha_format, bool include_alpha);

// RL: Render Loop
// RT: Render Thread

// Passed from the RL to the RT when a window is rendeirng on screen
// and should be added to the render loop.
const QEvent::Type WM_Expose            = QEvent::Type(QEvent::User + 1);

// Passed from the RL to the RT when a window is removed obscured and
// should be removed from the render loop.
const QEvent::Type WM_Obscure           = QEvent::Type(QEvent::User + 2);

// Passed from the RL to itself to initiate a polishAndSync() call.
const QEvent::Type WM_LockAndSync       = QEvent::Type(QEvent::User + 3);

// Passed from the RL to RT when GUI has been locked, waiting for sync
// (updatePaintNode())
const QEvent::Type WM_RequestSync       = QEvent::Type(QEvent::User + 4);

// Passed by the RT to itself to trigger another render pass. This is
// typically a result of QQuickWindow::update().
const QEvent::Type WM_RequestRepaint    = QEvent::Type(QEvent::User + 5);

// Passed by the RL to the RT when a window has changed size.
const QEvent::Type WM_Resize            = QEvent::Type(QEvent::User + 6);

// Passed by the RL to the RT to free up maybe release SG and GL contexts
// if no windows are rendering.
const QEvent::Type WM_TryRelease        = QEvent::Type(QEvent::User + 7);

// Passed by the RL to the RL when maybeUpdate is called on the RT to
// just replay the maybeUpdate later. This typically happens when
// updatePaintNode() results in a call to QQuickItem::update().
const QEvent::Type WM_UpdateLater       = QEvent::Type(QEvent::User + 8);

// Passed by the RL to the RT when a QQuickWindow::grabWindow() is
// called.
const QEvent::Type WM_Grab              = QEvent::Type(QEvent::User + 9);

// Passed by the RT to the RL to trigger animations to be advanced.
const QEvent::Type WM_AdvanceAnimations = QEvent::Type(QEvent::User + 10);

template <typename T> T *windowFor(const QList<T> list, QQuickWindow *window)
{
    for (int i=0; i<list.size(); ++i) {
        const T &t = list.at(i);
        if (t.window == window)
            return const_cast<T *>(&t);
    }
    return 0;
}


class WMWindowEvent : public QEvent
{
public:
    WMWindowEvent(QQuickWindow *c, QEvent::Type type) : QEvent(type), window(c) { }
    QQuickWindow *window;
};

class WMTryReleaseEvent : public WMWindowEvent
{
public:
    WMTryReleaseEvent(QQuickWindow *win, bool destroy)
        : WMWindowEvent(win, WM_TryRelease)
        , inDestructor(destroy)
    {}

    bool inDestructor;
};

class WMResizeEvent : public WMWindowEvent
{
public:
    WMResizeEvent(QQuickWindow *c, const QSize &s) : WMWindowEvent(c, WM_Resize), size(s) { }
    QSize size;
};


class WMExposeEvent : public WMWindowEvent
{
public:
    WMExposeEvent(QQuickWindow *c) : WMWindowEvent(c, WM_Expose), size(c->size()) { }
    QSize size;
};


class WMGrabEvent : public WMWindowEvent
{
public:
    WMGrabEvent(QQuickWindow *c, QImage *result) : WMWindowEvent(c, WM_Grab), image(result) {}
    QImage *image;
};


class QSGRenderThread : public QThread
{
    Q_OBJECT
public:

    QSGRenderThread(QSGThreadedRenderLoop *w)
        : wm(w)
        , gl(0)
        , sg(QSGContext::createDefaultContext())
        , pendingUpdate(0)
        , sleeping(false)
        , animationRunning(false)
        , guiIsLocked(false)
        , shouldExit(false)
        , allowMainThreadProcessing(true)
        , animationRequestsPending(0)
    {
        sg->moveToThread(this);
    }


    void invalidateOpenGL(QQuickWindow *window, bool inDestructor);
    void initializeOpenGL();

    bool event(QEvent *);
    void run();

    void syncAndRender();
    void sync();

    void requestRepaint()
    {
        if (sleeping)
            exit();
        if (m_windows.size() > 0)
            pendingUpdate |= RepaintRequest;
    }

public slots:
    void animationStarted() {
        RLDEBUG("    Render: animationStarted()");
        animationRunning = true;
        if (sleeping)
            exit();
    }

    void animationStopped() {
        RLDEBUG("    Render: animationStopped()");
        animationRunning = false;
    }

public:
    enum UpdateRequest {
        SyncRequest         = 0x01,
        RepaintRequest      = 0x02
    };

    QSGThreadedRenderLoop *wm;
    QOpenGLContext *gl;
    QSGContext *sg;

    QEventLoop eventLoop;

    uint pendingUpdate : 2;
    uint sleeping : 1;
    uint animationRunning : 1;

    volatile bool guiIsLocked;
    volatile bool shouldExit;

    volatile bool allowMainThreadProcessing;
    volatile int animationRequestsPending;

    QMutex mutex;
    QWaitCondition waitCondition;

    QElapsedTimer m_timer;

    struct Window {
        QQuickWindow *window;
        QSize size;
    };
    QList<Window> m_windows;
};

bool QSGRenderThread::event(QEvent *e)
{
    switch ((int) e->type()) {

    case WM_Expose: {
        RLDEBUG1("    Render: WM_Expose");
        WMExposeEvent *se = static_cast<WMExposeEvent *>(e);

        if (windowFor(m_windows, se->window)) {
            RLDEBUG1("    Render:  - window already added...");
            return true;
        }

        Window window;
        window.window = se->window;
        window.size = se->size;
        m_windows << window;
        return true; }

    case WM_Obscure: {
        RLDEBUG1("    Render: WM_Obscure");
        WMWindowEvent *ce = static_cast<WMWindowEvent *>(e);
        for (int i=0; i<m_windows.size(); ++i) {
            if (m_windows.at(i).window == ce->window) {
                RLDEBUG1("    Render:  - removed one...");
                m_windows.removeAt(i);
                break;
            }
        }

        if (sleeping && m_windows.size())
            exit();

        return true; }

    case WM_RequestSync:
        RLDEBUG("    Render: WM_RequestSync");
        if (sleeping)
            exit();
        if (m_windows.size() > 0)
            pendingUpdate |= SyncRequest;
        return true;

    case WM_Resize: {
        RLDEBUG("    Render: WM_Resize");
        WMResizeEvent *re = static_cast<WMResizeEvent *>(e);
        Window *w = windowFor(m_windows, re->window);
        w->size = re->size;
        // No need to wake up here as we will get a sync shortly.. (see QSGThreadedRenderLoop::resize());
        return true; }

    case WM_TryRelease:
        RLDEBUG1("    Render: WM_TryRelease");
        mutex.lock();
        if (m_windows.size() == 0) {
            WMTryReleaseEvent *wme = static_cast<WMTryReleaseEvent *>(e);
            RLDEBUG1("    Render:  - setting exit flag and invalidating GL");
            invalidateOpenGL(wme->window, wme->inDestructor);
            shouldExit = !gl;
            if (sleeping)
                exit();
        } else {
            RLDEBUG1("    Render:  - not releasing anything because we have active windows...");
        }
        waitCondition.wakeOne();
        mutex.unlock();
        return true;

    case WM_Grab: {
        RLDEBUG1("    Render: WM_Grab");
        WMGrabEvent *ce = static_cast<WMGrabEvent *>(e);
        Window *w = windowFor(m_windows, ce->window);
        mutex.lock();
        if (w) {
            gl->makeCurrent(ce->window);

            RLDEBUG1("    Render: - syncing scene graph");
            QQuickWindowPrivate *d = QQuickWindowPrivate::get(w->window);
            d->syncSceneGraph();

            RLDEBUG1("    Render: - rendering scene graph");
            QQuickWindowPrivate::get(ce->window)->renderSceneGraph(w->size);

            RLDEBUG1("    Render: - grabbing result...");
            *ce->image = qt_gl_read_framebuffer(w->size, false, false);
        }
        RLDEBUG1("    Render:  - waking gui to handle grab result");
        waitCondition.wakeOne();
        mutex.unlock();
        return true;
    }

    default:
        break;
    }
    return QThread::event(e);
}

void QSGRenderThread::invalidateOpenGL(QQuickWindow *window, bool inDestructor)
{
    RLDEBUG1("    Render: invalidateOpenGL()");

    if (!gl)
        return;

    if (!window) {
        qWarning("QSGThreadedRenderLoop:QSGRenderThread: no window to make current...");
        return;
    }


    bool persistentGL = false;
    bool persistentSG = false;

    // GUI is locked so accessing the wm and window here is safe
    for (int i=0; i<wm->m_windows.size(); ++i) {
        const QSGThreadedRenderLoop::Window &w = wm->m_windows.at(i);
        if (!inDestructor || w.window != window) {
            persistentSG |= w.window->isPersistentSceneGraph();
            persistentGL |= w.window->isPersistentOpenGLContext();
        }
    }

    gl->makeCurrent(window);

    // The canvas nodes must be cleanded up regardless if we are in the destructor..
    if (!persistentSG || inDestructor) {
        QQuickWindowPrivate *dd = QQuickWindowPrivate::get(window);
        dd->cleanupNodesOnShutdown();
    }

    // We're not doing any cleanup in this case...
    if (persistentSG) {
        RLDEBUG1("    Render:  - persistent SG, avoiding cleanup");
        return;
    }

    sg->invalidate();
    QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
    gl->doneCurrent();
    RLDEBUG1("    Render:  - invalidated scenegraph..");

    if (!persistentGL) {
        delete gl;
        gl = 0;
        RLDEBUG1("    Render:  - invalidated OpenGL");
    } else {
        RLDEBUG1("    Render:  - persistent GL, avoiding cleanup");
    }
}

void QSGRenderThread::initializeOpenGL()
{
    RLDEBUG1("    Render: initializeOpenGL()");
    QWindow *win = m_windows.at(0).window;
    bool temp = false;

    // Workaround for broken expose logic... We should not get an
    // expose when the size of a window is invalid, but we sometimes do.
    // On Mac this leads to harmless, yet annoying, console warnings
    if (m_windows.at(0).size.isEmpty()) {
        temp = true;
        win = new QWindow();
        win->setFormat(m_windows.at(0).window->requestedFormat());
        win->setSurfaceType(QWindow::OpenGLSurface);
        win->setGeometry(0, 0, 64, 64);
        win->create();
    }

    gl = new QOpenGLContext();
    // Pick up the surface format from one of them
    gl->setFormat(win->requestedFormat());
    gl->create();
    if (!gl->makeCurrent(win))
        qWarning("QQuickWindow: makeCurrent() failed...");
    sg->initialize(gl);

    if (temp) {
        delete win;
    }
}

/*!
    Enters the mutex lock to make sure GUI is blocking and performs
    sync, then wakes GUI.
 */
void QSGRenderThread::sync()
{
    RLDEBUG("    Render: sync()");
    mutex.lock();

    Q_ASSERT_X(guiIsLocked, "QSGRenderThread::sync()", "sync triggered on bad terms as gui is not already locked...");
    pendingUpdate = 0;

    for (int i=0; i<m_windows.size(); ++i) {
        Window &w = const_cast<Window &>(m_windows.at(i));
        if (w.size.width() == 0 || w.size.height() == 0) {
            RLDEBUG("    Render:  - window has bad size, waiting...");
            continue;
        }
        gl->makeCurrent(w.window);
        QQuickWindowPrivate *d = QQuickWindowPrivate::get(w.window);
        d->syncSceneGraph();
    }

    RLDEBUG("    Render:  - unlocking after sync");

    waitCondition.wakeOne();
    mutex.unlock();
}


void QSGRenderThread::syncAndRender()
{
#ifndef QSG_NO_WINDOW_TIMING
    if (qquick_window_timing)
        sinceLastTime = threadTimer.restart();
#endif
    RLDEBUG("    Render: syncAndRender()");

    // This animate request will get there after the sync
    if (animationRunning && animationRequestsPending < 2) {
        RLDEBUG("    Render:  - posting animate to gui..");
        ++animationRequestsPending;
        QCoreApplication::postEvent(wm, new QEvent(WM_AdvanceAnimations));

    }

    if (pendingUpdate & SyncRequest) {
        RLDEBUG("    Render:  - update pending, doing sync");
        sync();
    }

#ifndef QSG_NO_WINDOW_TIMING
    if (qquick_window_timing)
        syncTime = threadTimer.elapsed();
#endif

    for (int i=0; i<m_windows.size(); ++i) {
        Window &w = const_cast<Window &>(m_windows.at(i));
        QQuickWindowPrivate *d = QQuickWindowPrivate::get(w.window);
        if (!d->renderer || w.size.width() == 0 || w.size.height() == 0) {
            RLDEBUG("    Render:  - Window not yet ready, skipping render...");
            continue;
        }
        gl->makeCurrent(w.window);
        d->renderSceneGraph(w.size);
#ifndef QSG_NO_WINDOW_TIMING
        if (qquick_window_timing && i == 0)
            renderTime = threadTimer.elapsed();
#endif
        gl->swapBuffers(w.window);
        d->fireFrameSwapped();
    }
    RLDEBUG("    Render:  - rendering done");

#ifndef QSG_NO_WINDOW_TIMING
        if (qquick_window_timing)
            qDebug("window Time: sinceLast=%d, sync=%d, first render=%d, after final swap=%d",
                   sinceLastTime,
                   syncTime,
                   renderTime - syncTime,
                   threadTimer.elapsed() - renderTime);
#endif
}

void QSGRenderThread::run()
{
    RLDEBUG1("    Render: run()");
    while (!shouldExit) {

        if (m_windows.size() > 0) {
            if (!gl)
                initializeOpenGL();
            if (!sg->isReady())
                sg->initialize(gl);
            syncAndRender();
        }

        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);

        if (!shouldExit
            && ((!animationRunning && pendingUpdate == 0) || m_windows.size() == 0)) {
            RLDEBUG("    Render: enter event loop (going to sleep)");
            sleeping = true;
            exec();
            sleeping = false;
        }

    }

    Q_ASSERT_X(!gl, "QSGRenderThread::run()", "The OpenGL context should be cleaned up before exiting the render thread...");

    RLDEBUG1("    Render: run() completed...");
}

QSGThreadedRenderLoop::QSGThreadedRenderLoop()
    : m_animation_timer(0)
    , m_update_timer(0)
{
    m_thread = new QSGRenderThread(this);
    m_thread->moveToThread(m_thread);

    m_animation_driver = m_thread->sg->createAnimationDriver(this);

    m_exhaust_delay = get_env_int("QML_EXHAUST_DELAY", 5);

    connect(m_animation_driver, SIGNAL(started()), m_thread, SLOT(animationStarted()));
    connect(m_animation_driver, SIGNAL(stopped()), m_thread, SLOT(animationStopped()));
    connect(m_animation_driver, SIGNAL(started()), this, SLOT(animationStarted()));
    connect(m_animation_driver, SIGNAL(stopped()), this, SLOT(animationStopped()));

    m_animation_driver->install();
    RLDEBUG1("GUI: QSGThreadedRenderLoop() created");
}

QAnimationDriver *QSGThreadedRenderLoop::animationDriver() const
{
    return m_animation_driver;
}

QSGContext *QSGThreadedRenderLoop::sceneGraphContext() const
{
    return m_thread->sg;
}

bool QSGThreadedRenderLoop::anyoneShowing()
{
    for (int i=0; i<m_windows.size(); ++i) {
        QQuickWindow *c = m_windows.at(i).window;
        if (c->isVisible() && c->isExposed())
            return true;
    }
    return false;
}

void QSGThreadedRenderLoop::animationStarted()
{
    RLDEBUG("GUI: animationStarted()");
    if (!anyoneShowing() && m_animation_timer == 0)
        m_animation_timer = startTimer(qsgrl_animation_interval());
}

void QSGThreadedRenderLoop::animationStopped()
{
    RLDEBUG("GUI: animationStopped()");
    if (!anyoneShowing()) {
        killTimer(m_animation_timer);
        m_animation_timer = 0;
    }
}



/*
    Adds this window to the list of tracked windowes in this window
    manager. show() does not trigger rendering to start, that happens
    in expose.
 */

void QSGThreadedRenderLoop::show(QQuickWindow *window)
{
    RLDEBUG1("GUI: show()");

    Window win;
    win.window = window;
    win.pendingUpdate = false;
    m_windows << win;
}



/*
    Removes this window from the list of tracked windowes in this
    window manager. hide() will trigger obscure, which in turn will
    stop rendering.
 */

void QSGThreadedRenderLoop::hide(QQuickWindow *window)
{
    RLDEBUG1("GUI: hide()");

    if (window->isExposed())
        handleObscurity(window);

    releaseResources(window);

    for (int i=0; i<m_windows.size(); ++i) {
        if (m_windows.at(i).window == window) {
            m_windows.removeAt(i);
            break;
        }
    }
}


/*!
    If the window is first hide it, then perform a complete cleanup
    with releaseResources which will take down the GL context and
    exit the rendering thread.
 */
void QSGThreadedRenderLoop::windowDestroyed(QQuickWindow *window)
{
    RLDEBUG1("GUI: windowDestroyed()");

    if (window->isVisible())
        hide(window);
    releaseResources(window, true);

    RLDEBUG1("GUI:  - done with windowDestroyed()");
}


void QSGThreadedRenderLoop::exposureChanged(QQuickWindow *window)
{
    RLDEBUG1("GUI: exposureChanged()");
    if (windowFor(m_windows, window) == 0)
        return;

    if (window->isExposed()) {
        handleExposure(window);
    } else {
        handleObscurity(window);
    }
}


/*!
    Will post an event to the render thread that this window should
    start to render.
 */
void QSGThreadedRenderLoop::handleExposure(QQuickWindow *window)
{
    RLDEBUG1("GUI: handleExposure");

    // Because we are going to bind a GL context to it, make sure it
    // is created.
    if (!window->handle())
        window->create();

    QCoreApplication::postEvent(m_thread, new WMExposeEvent(window));

    // Start render thread if it is not running
    if (!m_thread->isRunning()) {
        m_thread->shouldExit = false;
        m_thread->animationRunning = m_animation_driver->isRunning();

        RLDEBUG1("GUI: - starting render thread...");
        m_thread->start();

    } else {
        RLDEBUG1("GUI: - render thread already running");
    }

    polishAndSync();

    // Kill non-visual animation timer if it is running
    if (m_animation_timer) {
        killTimer(m_animation_timer);
        m_animation_timer = 0;
    }

}

/*!
    This function posts an event to the render thread to remove the window
    from the list of windowses to render.

    It also starts up the non-vsync animation tick if no more windows
    are showing.
 */
void QSGThreadedRenderLoop::handleObscurity(QQuickWindow *window)
{
    RLDEBUG1("GUI: handleObscurity");
    if (m_thread->isRunning())
        QCoreApplication::postEvent(m_thread, new WMWindowEvent(window, WM_Obscure));

    if (!anyoneShowing() && m_animation_driver->isRunning() && m_animation_timer == 0) {
        m_animation_timer = startTimer(qsgrl_animation_interval());
    }
}


/*!
    Called whenever the QML scene has changed. Will post an event to
    ourselves that a sync is needed.
 */
void QSGThreadedRenderLoop::maybeUpdate(QQuickWindow *window)
{
    Q_ASSERT_X(QThread::currentThread() == QCoreApplication::instance()->thread() || m_thread->guiIsLocked,
               "QQuickItem::update()",
               "Function can only be called from GUI thread or during QQuickItem::updatePaintNode()");

    RLDEBUG("GUI: maybeUpdate...");
    Window *w = windowFor(m_windows, window);
    if (!w || w->pendingUpdate || !m_thread->isRunning()) {
        return;
    }

    // Call this function from the Gui thread later as startTimer cannot be
    // called from the render thread.
    if (QThread::currentThread() == m_thread) {
        RLDEBUG("GUI:  - on render thread, posting update later");
        QCoreApplication::postEvent(this, new WMWindowEvent(window, WM_UpdateLater));
        return;
    }


    w->pendingUpdate = true;

    if (m_update_timer > 0) {
        return;
    }

    RLDEBUG("GUI:  - posting update");
    m_update_timer = startTimer(m_animation_driver->isRunning() ? m_exhaust_delay : 0, Qt::PreciseTimer);
}

/*!
    Called when the QQuickWindow should be explicitly repainted. This function
    can also be called on the render thread when the GUI thread is blocked to
    keep render thread animations alive.
 */
void QSGThreadedRenderLoop::update(QQuickWindow *window)
{
    if (QThread::currentThread() == m_thread) {
        RLDEBUG("Gui: update called on render thread");
        m_thread->requestRepaint();
        return;
    }

    RLDEBUG("Gui: update called");
    maybeUpdate(window);
}



/*!
 * Release resources will post an event to the render thread to
 * free up the SG and GL resources and exists the render thread.
 */
void QSGThreadedRenderLoop::releaseResources(QQuickWindow *window, bool inDestructor)
{
    RLDEBUG1("GUI: releaseResources requested...");

    m_thread->mutex.lock();
    if (m_thread->isRunning() && !m_thread->shouldExit) {
        RLDEBUG1("GUI:  - posting release request to render thread");
        QCoreApplication::postEvent(m_thread, new WMTryReleaseEvent(window, inDestructor));
        m_thread->waitCondition.wait(&m_thread->mutex);
    }
    m_thread->mutex.unlock();
}



void QSGThreadedRenderLoop::polishAndSync()
{
    if (!anyoneShowing())
        return;

#ifndef QSG_NO_WINDOW_TIMING
    QElapsedTimer timer;
    int polishTime = 0;
    int waitTime = 0;
    if (qquick_window_timing)
        timer.start();
#endif
    RLDEBUG("GUI: polishAndSync()");
    // Polish as the last thing we do before we allow the sync to take place
    for (int i=0; i<m_windows.size(); ++i) {
        const Window &w = m_windows.at(i);
        QQuickWindowPrivate *d = QQuickWindowPrivate::get(w.window);
        d->polishItems();
    }
#ifndef QSG_NO_WINDOW_TIMING
    if (qquick_window_timing)
        polishTime = timer.elapsed();
#endif

    RLDEBUG("GUI:  - clearing update flags...");
    for (int i=0; i<m_windows.size(); ++i) {
        m_windows[i].pendingUpdate = false;
    }

    RLDEBUG("GUI:  - lock for sync...");
    m_thread->mutex.lock();
    m_thread->guiIsLocked = true;
    QEvent *event = new QEvent(WM_RequestSync);

    QCoreApplication::postEvent(m_thread, event);
    RLDEBUG("GUI:  - wait for sync...");
#ifndef QSG_NO_WINDOW_TIMING
    if (qquick_window_timing)
        waitTime = timer.elapsed();
#endif
    m_thread->waitCondition.wait(&m_thread->mutex);
    m_thread->guiIsLocked = false;
    m_thread->mutex.unlock();
    RLDEBUG("GUI:  - unlocked after sync...");

#ifndef QSG_NO_WINDOW_TIMING
    if (qquick_window_timing)
        qDebug(" - polish=%d, wait=%d, sync=%d", polishTime, waitTime - polishTime, int(timer.elapsed() - waitTime));
#endif
}

bool QSGThreadedRenderLoop::event(QEvent *e)
{
    switch ((int) e->type()) {

    case QEvent::Timer:
        if (static_cast<QTimerEvent *>(e)->timerId() == m_animation_timer) {
            RLDEBUG("Gui: QEvent::Timer -> non-visual animation");
            m_animation_driver->advance();
        } else if (static_cast<QTimerEvent *>(e)->timerId() == m_update_timer) {
            RLDEBUG("Gui: QEvent::Timer -> polishAndSync()");
            killTimer(m_update_timer);
            m_update_timer = 0;
            polishAndSync();
        }
        return true;

    case WM_UpdateLater: {
        QQuickWindow *window = static_cast<WMWindowEvent *>(e)->window;
        // The window might have gone away...
        if (windowFor(m_windows, window))
            maybeUpdate(window);
        return true; }

    case WM_AdvanceAnimations:
        --m_thread->animationRequestsPending;
        RLDEBUG("GUI: WM_AdvanceAnimations");
        if (m_animation_driver->isRunning()) {
#ifdef QQUICK_CANVAS_TIMING
            QElapsedTimer timer;
            timer.start();
#endif
            m_animation_driver->advance();
            RLDEBUG("GUI:  - animations advanced..");
#ifdef QQUICK_CANVAS_TIMING
            if (qquick_canvas_timing)
                qDebug(" - animation: %d", (int) timer.elapsed());
#endif
        }
        return true;

    default:
        break;
    }

    return QObject::event(e);
}



/*
    Locks down GUI and performs a grab the scene graph, then returns the result.

    Since the QML scene could have changed since the last time it was rendered,
    we need to polish and sync the scene graph. This might seem superfluous, but
     - QML changes could have triggered deleteLater() which could have removed
       textures or other objects from the scene graph, causing render to crash.
     - Autotests rely on grab(), setProperty(), grab(), compare behavior.
 */

QImage QSGThreadedRenderLoop::grab(QQuickWindow *window)
{
    RLDEBUG("GUI: grab");
    if (!m_thread->isRunning())
        return QImage();

    if (!window->handle())
        window->create();

    RLDEBUG1("GUI: - polishing items...");
    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    d->polishItems();

    QImage result;
    m_thread->mutex.lock();
    RLDEBUG1("GUI: - locking, posting grab event");
    QCoreApplication::postEvent(m_thread, new WMGrabEvent(window, &result));
    m_thread->waitCondition.wait(&m_thread->mutex);
    RLDEBUG1("GUI: - locking, grab done, unlocking");
    m_thread->mutex.unlock();

    RLDEBUG1("Gui: - grab complete");

    return result;
}

/*
    Notify the render thread that the window is now a new size. Then
    locks GUI until render has adapted.
 */

void QSGThreadedRenderLoop::resize(QQuickWindow *w, const QSize &size)
{
    RLDEBUG1("GUI: resize");

    if (!m_thread->isRunning() || !m_windows.size() || !w->isExposed() || windowFor(m_windows, w) == 0) {
        return;
    }

    if (size.width() == 0 || size.height() == 0)
        return;

    RLDEBUG("GUI:  - posting resize event...");
    WMResizeEvent *e = new WMResizeEvent(w, size);
    QCoreApplication::postEvent(m_thread, e);

    polishAndSync();
}

#include "qsgthreadedrenderloop.moc"

QT_END_NAMESPACE
