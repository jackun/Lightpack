/*
 * KMSGrabber.hpp
 *
 *  Created on: 20.04.20
 *     Project: Lightpack
 *
 *  Copyright (c) 2011 Andrey Isupov, Timur Sattarov, Mike Shatohin
 *
 *  Lightpack a USB content-driving ambient lighting system
 *
 *  Lightpack is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Lightpack is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "GrabberBase.hpp"
#include "../src/enums.hpp"

#ifdef X11_GRAB_SUPPORT

#include <QScopedPointer>
#include "../src/debug.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

struct X11GrabberData;
struct _XDisplay;
struct dmabuf_source_t;

using namespace Grab;

class KMSGrabber : public GrabberBase
{
public:
    KMSGrabber(QObject *parent, GrabberContext *context);
    virtual ~KMSGrabber();

    DECLARE_GRABBER_NAME("KMSGrabber")

protected:
    virtual GrabResult grabScreens();
    virtual bool reallocate(const QList<ScreenInfo> &screens);
    virtual QList<ScreenInfo> * screensWithWidgets(QList<ScreenInfo> *result, const QList<GrabWidget *> &grabWidgets);

private:
    void freeScreens();

private:
    //_XDisplay *_display;
    dmabuf_source_t * m_dmabuf = nullptr;

    GLuint program = 0;
    GLuint vertexshader = 0;
    GLuint fragmentshader = 0;
    GLuint vertexarray = 0;
    GLuint vertexbuffer = 0;

    void *m_display = 0; //EGLDisplay
    void *m_context = 0;
    void *m_fbconfig = 0;
    void *m_surface = 0;
};
#endif // X11_GRAB_SUPPORT
