/******************************************************************************
 * $Id: gps.h, v1.0 2010/08/26 SethDart Exp $
 *
 * Project:  OpenCPN
 * Purpose:  DashBoard Plugin
 * Author:   Jean-Eudes Onfray
 *
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register   *
 *   $EMAIL$   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 */

#ifndef __GPS_H__
#define __GPS_H__

// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

// for all others, include the necessary headers (this file is usually all you
// need because it includes almost all "standard" wxWidgets headers)
#ifndef WX_PRECOMP
    #include <wx/wx.h>
#endif

#include "instrument.h"

// Required for struct SAT_INFO
#include "nmea0183/SatInfo.h"

class DashboardInstrument_GPS: public DashboardInstrument
{
      public:
            DashboardInstrument_GPS( wxWindow *parent, wxWindowID id, wxString title);

            ~DashboardInstrument_GPS(void){}

            void SetSatInfo(int cnt, int seq, SAT_INFO sats[4]);

      private:

      protected:
            wxString m_title;
            int m_cx, m_cy, m_radius;
            int m_SatCount;
            SAT_INFO m_SatInfo[12];

            void Draw(wxBufferedDC* dc);
            void DrawFrame(wxBufferedDC* dc);
            void DrawBackground(wxBufferedDC* dc);
            void DrawForeground(wxBufferedDC* dc);
};

#endif // __GPS_H__
