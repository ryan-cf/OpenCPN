/******************************************************************************
 *
 * Project:  OpenCPN
 *
 ***************************************************************************
 *   Copyright (C) 2013 by David S. Register                               *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 ***************************************************************************
 */

#include "wx/wxprec.h"

#include "Quilt.h"
#include "chartdb.h"
#include "s52plib.h"
#include "chcanv.h"
#include "s57chart.h"
#include "ocpn_pixel.h"                         // for ocpnUSE_DIBSECTION


#include <wx/listimpl.cpp>
WX_DEFINE_LIST( PatchList );

extern ChartDB *ChartData;
extern ArrayOfInts g_quilt_noshow_index_array;
extern s52plib *ps52plib;
extern ChartStack *pCurrentStack;
extern ChartCanvas *cc1;
extern int g_GroupIndex;
extern ColorScheme global_color_scheme;

static int CompareScales( QuiltCandidate *qc1, QuiltCandidate *qc2 )
{
    if( !ChartData ) return 0;

    const ChartTableEntry &cte1 = ChartData->GetChartTableEntry( qc1->dbIndex );
    const ChartTableEntry &cte2 = ChartData->GetChartTableEntry( qc2->dbIndex );

    if( cte1.GetScale() == cte2.GetScale() )          // same scales, so sort on dbIndex
        return qc1->dbIndex - qc2->dbIndex;
    else
        return cte1.GetScale() - cte2.GetScale();
}

Quilt::Quilt()
{
//      m_bEnableRaster = true;
//      m_bEnableVector = false;;
//      m_bEnableCM93 = false;

    m_reference_scale = 1;
    m_refchart_dbIndex = -1;
    m_reference_type = CHART_TYPE_UNKNOWN;
    m_reference_family = CHART_FAMILY_UNKNOWN;

    cnode = NULL;

    m_pBM = NULL;
    m_bcomposed = false;
    m_bbusy = false;

    m_pcandidate_array = new ArrayOfSortedQuiltCandidates( CompareScales );
    m_nHiLiteIndex = -1;

}

Quilt::~Quilt()
{
    m_PatchList.DeleteContents( true );
    m_PatchList.Clear();

    EmptyCandidateArray();
    delete m_pcandidate_array;

    m_extended_stack_array.Clear();

    delete m_pBM;
}

bool Quilt::IsVPBlittable( ViewPort &VPoint, int dx, int dy, bool b_allow_vector )
{
    bool ret_val = true;
    if( m_vp_rendered.IsValid() ) {
        wxPoint2DDouble p1 = VPoint.GetDoublePixFromLL( m_vp_rendered.clat, m_vp_rendered.clon );
        wxPoint2DDouble p2 = VPoint.GetDoublePixFromLL( VPoint.clat, VPoint.clon );
        double deltax = p2.m_x - p1.m_x;
        double deltay = p2.m_y - p1.m_y;

        ChartBase *pch = GetFirstChart();
        while( pch ) {
            if( pch->GetChartFamily() == CHART_FAMILY_RASTER ) {
                if( ( fabs( deltax - dx ) > 1e-2 ) || ( fabs( deltay - dy ) > 1e-2 ) ) {
                    ret_val = false;
                    break;
                }

            } else {
                if( !b_allow_vector ) {
                    ret_val = false;
                    break;
                } else if( ( fabs( deltax - dx ) > 1e-2 ) || ( fabs( deltay - dy ) > 1e-2 ) ) {
                    ret_val = false;
                    break;
                }

            }

            pch = GetNextChart();
        }
    } else
        ret_val = false;

    return ret_val;
}

bool Quilt::IsChartQuiltableRef( int db_index )
{
    if( db_index < 0 ) return false;

    //    Is the chart targeted by db_index useable as a quilt reference chart?
    const ChartTableEntry &ctei = ChartData->GetChartTableEntry( db_index );

    bool bproj_match = true;                  // Accept all projections

    double skew_norm = ctei.GetChartSkew();
    if( skew_norm > 180. ) skew_norm -= 360.;

    bool skew_match = fabs( skew_norm ) < 1.;  // Only un-skewed charts are acceptable for quilt

    //    In noshow array?
    bool b_noshow = false;
    for( unsigned int i = 0; i < g_quilt_noshow_index_array.GetCount(); i++ ) {
        if( g_quilt_noshow_index_array.Item( i ) == db_index )        // chart is in the noshow list
        {
            b_noshow = true;
            break;
        }
    }

    return ( bproj_match & skew_match & !b_noshow );
}

bool Quilt::IsChartInQuilt( ChartBase *pc )
{
    //    Iterate thru the quilt
    for( unsigned int ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
        QuiltCandidate *pqc = m_pcandidate_array->Item( ir );
        if( ( pqc->b_include ) && ( !pqc->b_eclipsed ) ) {
            if( ChartData->OpenChartFromDB( pqc->dbIndex, FULL_INIT ) == pc ) return true;
        }
    }
    return false;
}

ArrayOfInts Quilt::GetCandidatedbIndexArray( bool from_ref_chart, bool exclude_user_hidden )
{
    ArrayOfInts ret;
    for( unsigned int ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
        QuiltCandidate *pqc = m_pcandidate_array->Item( ir );
        if( from_ref_chart )                     // only add entries of smaller scale than ref scale
        {
            if( pqc->ChartScale >= m_reference_scale ) {
                // Search the no-show array
                if( exclude_user_hidden ) {
                    bool b_noshow = false;
                    for( unsigned int i = 0; i < g_quilt_noshow_index_array.GetCount(); i++ ) {
                        if( g_quilt_noshow_index_array.Item( i ) == pqc->dbIndex ) // chart is in the noshow list
                        {
                            b_noshow = true;
                            break;
                        }
                    }
                    if( !b_noshow ) ret.Add( pqc->dbIndex );
                } else {
                    ret.Add( pqc->dbIndex );
                }
            }
        } else
            ret.Add( pqc->dbIndex );

    }
    return ret;
}

QuiltPatch *Quilt::GetCurrentPatch()
{
    if( cnode ) return ( cnode->GetData() );
    else
        return NULL;
}

void Quilt::EmptyCandidateArray( void )
{
    for( unsigned int i = 0; i < m_pcandidate_array->GetCount(); i++ ) {
        delete m_pcandidate_array->Item( i );
    }

    m_pcandidate_array->Clear();

}

ChartBase *Quilt::GetFirstChart()
{
    if( !ChartData ) return NULL;

    if( !ChartData->IsValid() ) // This could happen during yield recursion from progress dialog during databse update
        return NULL;

    if( !m_bcomposed ) return NULL;

    if( m_bbusy ) return NULL;

    m_bbusy = true;
    ChartBase *pret = NULL;
    cnode = m_PatchList.GetFirst();
    while( cnode && !cnode->GetData()->b_Valid )
        cnode = cnode->GetNext();
    if( cnode && cnode->GetData()->b_Valid ) pret = ChartData->OpenChartFromDB(
                    cnode->GetData()->dbIndex, FULL_INIT );

    m_bbusy = false;
    return pret;
}

ChartBase *Quilt::GetNextChart()
{
    if( !ChartData ) return NULL;

    if( !ChartData->IsValid() ) return NULL;

    if( m_bbusy ) return NULL;

    m_bbusy = true;
    ChartBase *pret = NULL;
    if( cnode ) {
        cnode = cnode->GetNext();
        while( cnode && !cnode->GetData()->b_Valid )
            cnode = cnode->GetNext();
        if( cnode && cnode->GetData()->b_Valid ) pret = ChartData->OpenChartFromDB(
                        cnode->GetData()->dbIndex, FULL_INIT );
    }

    m_bbusy = false;
    return pret;
}

ChartBase *Quilt::GetLargestScaleChart()
{
    if( !ChartData ) return NULL;

    if( m_bbusy ) return NULL;

    m_bbusy = true;
    ChartBase *pret = NULL;
    cnode = m_PatchList.GetLast();
    if( cnode ) pret = ChartData->OpenChartFromDB( cnode->GetData()->dbIndex, FULL_INIT );

    m_bbusy = false;
    return pret;
}

wxRegion Quilt::GetChartQuiltRegion( const ChartTableEntry &cte, ViewPort &vp )
{
    wxRegion chart_region;
    wxRegion screen_region( vp.rv_rect );

    // Special case for charts which extend around the world, or near to it
    //  Mostly this means cm93....
    //  Take the whole screen, clipped at +/- 80 degrees lat
    if(fabs(cte.GetLonMax() - cte.GetLonMin()) > 180.) {
        int n_ply_entries = 4;
        float ply[8];
        ply[0] = 80.;
        ply[1] = vp.GetBBox().GetMinX();
        ply[2] = 80.;
        ply[3] = vp.GetBBox().GetMaxX();
        ply[4] = -80.;
        ply[5] = vp.GetBBox().GetMaxX();
        ply[6] = -80.;
        ply[7] = vp.GetBBox().GetMinX();


        wxRegion t_region = vp.GetVPRegionIntersect( screen_region, 4, &ply[0],
                                                     cte.GetScale() );
        return t_region;
    }


    //    If the chart has an aux ply table, use it for finer region precision
    int nAuxPlyEntries = cte.GetnAuxPlyEntries();
    if( nAuxPlyEntries >= 1 ) {
        for( int ip = 0; ip < nAuxPlyEntries; ip++ ) {
            float *pfp = cte.GetpAuxPlyTableEntry( ip );
            int nAuxPly = cte.GetAuxCntTableEntry( ip );

            wxRegion t_region = vp.GetVPRegionIntersect( screen_region, nAuxPly, pfp,
                                cte.GetScale() );
            if( !t_region.Empty() )
                chart_region.Union( t_region );
        }
    }

    else {
        int n_ply_entries = cte.GetnPlyEntries();
        float *pfp = cte.GetpPlyTable();

        if( n_ply_entries >= 3 ) // could happen with old database and some charts, e.g. SHOM 2381.kap
        {
            wxRegion t_region = vp.GetVPRegionIntersect( screen_region, n_ply_entries, pfp,
                                cte.GetScale() );
            if( !t_region.Empty() )
                chart_region.Union( t_region );

        } else
            chart_region = screen_region;
    }

    //  Remove the NoCovr regions
    int nNoCovrPlyEntries = cte.GetnNoCovrPlyEntries();
    if( nNoCovrPlyEntries ) {
        for( int ip = 0; ip < nNoCovrPlyEntries; ip++ ) {
            float *pfp = cte.GetpNoCovrPlyTableEntry( ip );
            int nNoCovrPly = cte.GetNoCovrCntTableEntry( ip );

            wxRegion t_region = vp.GetVPRegionIntersect( screen_region, nNoCovrPly, pfp,
                                                         cte.GetScale() );

            //  We do a test removal of the NoCovr region.
            //  If the result iz empty, it must be that the NoCovr region is
            //  the full extent M_COVR(CATCOV=2) feature found in NOAA ENCs.
            //  We ignore it.

            if(!t_region.IsEmpty()) {
                wxRegion test_region = chart_region;
                test_region.Subtract( t_region );

                if( !test_region.IsEmpty())
                    chart_region = test_region;
            }
        }
    }


    //    Another superbad hack....
    //    Super small scale raster charts like bluemarble.kap usually cross the prime meridian
    //    and Plypoints georef is problematic......
    //    So, force full screen coverage in the quilt
    if( (cte.GetScale() > 90000000) && (cte.GetChartFamily() == CHART_FAMILY_RASTER) )
        chart_region = screen_region;

    //    Clip the region to the current viewport
    chart_region.Intersect( vp.rv_rect );

    if( chart_region.IsOk() ) return chart_region;
    else
        return wxRegion( 0, 0, 100, 100 );
}

bool Quilt::IsQuiltVector( void )
{
    if( m_bbusy ) return false;

    m_bbusy = true;

    bool ret = false;

    wxPatchListNode *cnode = m_PatchList.GetFirst();
    while( cnode ) {
        if( cnode->GetData() ) {
            QuiltPatch *pqp = cnode->GetData();

            if( ( pqp->b_Valid ) && ( !pqp->b_eclipsed ) ) {
                const ChartTableEntry &ctei = ChartData->GetChartTableEntry( pqp->dbIndex );

                if( ctei.GetChartFamily() == CHART_FAMILY_VECTOR ) {
                    ret = true;
                    break;
                }

            }
        }
        cnode = cnode->GetNext();
    }

    m_bbusy = false;
    return ret;
}

int Quilt::GetChartdbIndexAtPix( wxPoint p )
{
    if( m_bbusy ) return -1;

    m_bbusy = true;

    int ret = -1;

    wxPatchListNode *cnode = m_PatchList.GetFirst();
    while( cnode ) {
        if( cnode->GetData()->ActiveRegion.Contains( p ) == wxInRegion ) {
            ret = cnode->GetData()->dbIndex;
            break;
        } else
            cnode = cnode->GetNext();
    }

    m_bbusy = false;
    return ret;
}

ChartBase *Quilt::GetChartAtPix( wxPoint p )
{
    if( m_bbusy ) return NULL;

    m_bbusy = true;

    //    The patchlist is organized from small to large scale.
    //    We generally will want the largest scale chart at this point, so
    //    walk the whole list.  The result will be the last one found, i.e. the largest scale chart.
    ChartBase *pret = NULL;
    wxPatchListNode *cnode = m_PatchList.GetFirst();
    while( cnode ) {
        QuiltPatch *pqp = cnode->GetData();
        if( !pqp->b_overlay && (pqp->ActiveRegion.Contains( p ) == wxInRegion) )
                pret = ChartData->OpenChartFromDB( pqp->dbIndex, FULL_INIT );
        cnode = cnode->GetNext();
    }

    m_bbusy = false;
    return pret;
}

ChartBase *Quilt::GetOverlayChartAtPix( wxPoint p )
{
    if( m_bbusy ) return NULL;

    m_bbusy = true;

    //    The patchlist is organized from small to large scale.
    //    We generally will want the largest scale chart at this point, so
    //    walk the whole list.  The result will be the last one found, i.e. the largest scale chart.
    ChartBase *pret = NULL;
    wxPatchListNode *cnode = m_PatchList.GetFirst();
    while( cnode ) {
        QuiltPatch *pqp = cnode->GetData();
        if( pqp->b_overlay && ( pqp->ActiveRegion.Contains( p ) == wxInRegion) )
                pret = ChartData->OpenChartFromDB( pqp->dbIndex, FULL_INIT );
        cnode = cnode->GetNext();
    }

    m_bbusy = false;
    return pret;
}


void Quilt::InvalidateAllQuiltPatchs( void )
{
    if( m_bbusy ) return;

    m_bbusy = true;
    m_bbusy = false;
    return;
}

ArrayOfInts Quilt::GetQuiltIndexArray( void )
{
    return m_index_array;

    ArrayOfInts ret;

    if( m_bbusy ) return ret;

    m_bbusy = true;

    wxPatchListNode *cnode = m_PatchList.GetFirst();
    while( cnode ) {
        ret.Add( cnode->GetData()->dbIndex );
        cnode = cnode->GetNext();
    }

    m_bbusy = false;

    return ret;
}

bool Quilt::IsQuiltDelta( ViewPort &vp )
{
    if( !m_vp_quilt.IsValid() || !m_bcomposed ) return true;

    if( m_vp_quilt.view_scale_ppm != vp.view_scale_ppm ) return true;

    //    Has the quilt shifted by more than one pixel in any direction?
    wxPoint cp_last, cp_this;

    cp_last = m_vp_quilt.GetPixFromLL( vp.clat, vp.clon );
    cp_this = vp.GetPixFromLL( vp.clat, vp.clon );

    return ( cp_last != cp_this );
}

void Quilt::AdjustQuiltVP( ViewPort &vp_last, ViewPort &vp_proposed )
{
    if( m_bbusy ) return;

//      ChartBase *pRefChart = GetLargestScaleChart();
    ChartBase *pRefChart = ChartData->OpenChartFromDB( m_refchart_dbIndex, FULL_INIT );

    if( pRefChart ) pRefChart->AdjustVP( vp_last, vp_proposed );
}

double Quilt::GetRefNativeScale()
{
    double ret_val = 1.0;
    if( ChartData ) {
        ChartBase *pc = ChartData->OpenChartFromDB( m_refchart_dbIndex, FULL_INIT );
        if( pc ) ret_val = pc->GetNativeScale();
    }

    return ret_val;
}

int Quilt::GetNewRefChart( void )
{
    //    Using the current quilt, select a useable reference chart
    //    Said chart will be in the extended (possibly full-screen) stack,
    //    And will have a scale equal to or just greater than the current quilt reference scale,
    //    And will match current quilt projection type, and
    //    will have Skew=0, so as to be fully quiltable
    int new_ref_dbIndex = m_refchart_dbIndex;
    unsigned int im = m_extended_stack_array.GetCount();
    if( im > 0 ) {
        for( unsigned int is = 0; is < im; is++ ) {
            const ChartTableEntry &m = ChartData->GetChartTableEntry( m_extended_stack_array.Item( is ) );
//                  if((m.GetScale() >= m_reference_scale) && (m_reference_type == m.GetChartType()))
            if( ( m.GetScale() >= m_reference_scale )
                    && ( m_reference_family == m.GetChartFamily() )
                    && ( m_quilt_proj == m.GetChartProjectionType() )
                    && ( m.GetChartSkew() == 0.0 ) ) {
                new_ref_dbIndex = m_extended_stack_array.Item( is );
                break;
            }
        }
    }
    return new_ref_dbIndex;
}

int Quilt::AdjustRefOnZoomOut( double proposed_scale_onscreen )
{
    //    If the reference chart is undefined, we really need to select one now.
    if( m_refchart_dbIndex < 0 ) {
        int new_ref_dbIndex = GetNewRefChart();
        SetReferenceChart( new_ref_dbIndex );
    }

    int new_db_index = m_refchart_dbIndex;

    unsigned int extended_array_count = m_extended_stack_array.GetCount();

    if( m_refchart_dbIndex >= 0 && ( extended_array_count > 0 ) ) {
        ChartBase *pc = ChartData->OpenChartFromDB( m_refchart_dbIndex, FULL_INIT );
        if( pc ) {
            int current_db_index = m_refchart_dbIndex;
            int current_family = m_reference_family;

            double max_ref_scale = pc->GetNormalScaleMax( m_canvas_scale_factor, m_canvas_width );

            if( proposed_scale_onscreen > max_ref_scale ) {
                m_zout_dbindex = -1;
                unsigned int target_stack_index = 0;
                int target_stack_index_check = m_extended_stack_array.Index( current_db_index ); // Lookup

                if( wxNOT_FOUND != target_stack_index_check ) target_stack_index =
                        target_stack_index_check;

                while( ( proposed_scale_onscreen > max_ref_scale )
                        && ( target_stack_index < ( extended_array_count - 1 ) ) ) {
                    target_stack_index++;
                    int test_db_index = m_extended_stack_array.Item( target_stack_index );

                    if( ( current_family == ChartData->GetDBChartFamily( test_db_index ) )
                            && IsChartQuiltableRef( test_db_index ) ) {
                        //    open the target, and check the min_scale
                        ChartBase *ptest_chart = ChartData->OpenChartFromDB( test_db_index,
                                                 FULL_INIT );
                        if( ptest_chart ) max_ref_scale = ptest_chart->GetNormalScaleMax(
                                                                  m_canvas_scale_factor, m_canvas_width );
                    }
                }

                bool b_ref_set = false;
                if( proposed_scale_onscreen > max_ref_scale) {          // could not find a useful chart

                    //  If cm93 is available, allow a one-time switch of chart family
                    //  and leave a bread crumb (m_zout_dbindex) to allow selecting this chart
                    //  and family on subsequent zoomin.
                    for( unsigned int ir = 0; ir < m_extended_stack_array.GetCount(); ir++ ) {
                        int i = m_extended_stack_array.Item(ir);        // chart index

                        if( ChartData->GetDBChartType( i ) == CHART_TYPE_CM93COMP )  {
                            target_stack_index = ir;
                            m_zout_dbindex = m_refchart_dbIndex;  //save for later
                            SetReferenceChart( i );
                            b_ref_set = true;
                            break;
                        }
                    }
                }

                if( !b_ref_set && (target_stack_index < extended_array_count) ) {
                    new_db_index = m_extended_stack_array.Item( target_stack_index );
                    if( ( current_family == ChartData->GetDBChartFamily( new_db_index ) )
                            && IsChartQuiltableRef( new_db_index ) )
                        SetReferenceChart( new_db_index );
                }
            }
        }
    }
    return m_refchart_dbIndex;
}

int Quilt::AdjustRefOnZoomIn( double proposed_scale_onscreen )
{
    //    If the reference chart is undefined, we really need to select one now.
    if( m_refchart_dbIndex < 0 ) {
        int new_ref_dbIndex = GetNewRefChart();
        SetReferenceChart( new_ref_dbIndex );
    }

    int new_db_index = m_refchart_dbIndex;
    int current_db_index = m_refchart_dbIndex;
    int current_family = m_reference_family;

    unsigned int extended_array_count = m_extended_stack_array.GetCount();

    if( m_refchart_dbIndex >= 0 && ( extended_array_count > 0 ) ) {
        ChartBase *pc = ChartData->OpenChartFromDB( m_refchart_dbIndex, FULL_INIT );
        if( pc ) {
            double min_ref_scale = pc->GetNormalScaleMin( m_canvas_scale_factor, false );

            //  If the current chart is cm93, and it became so due to a zout from another family,
            //  detect this case and allow switch to save chart index if the min/max scales comply
            if( ( pc->GetChartType() == CHART_TYPE_CM93COMP ) && (m_zout_dbindex >= 0) ) {
                min_ref_scale = proposed_scale_onscreen + 1;  // just to force the test below to pass
                current_family = ChartData->GetDBChartFamily( m_zout_dbindex );
            }

            if( proposed_scale_onscreen < min_ref_scale )  {

                unsigned int target_stack_index = 0;
                int target_stack_index_check = m_extended_stack_array.Index( current_db_index ); // Lookup

                if( wxNOT_FOUND != target_stack_index_check ) target_stack_index =
                        target_stack_index_check;

                while( ( proposed_scale_onscreen < min_ref_scale ) && ( target_stack_index > 0 ) ) {
                    target_stack_index--;
                    int test_db_index = m_extended_stack_array.Item( target_stack_index );

                    if( pCurrentStack->DoesStackContaindbIndex( test_db_index ) ) {
                        if( ( current_family == ChartData->GetDBChartFamily( test_db_index ) )
                                && IsChartQuiltableRef( test_db_index ) ) {

                            //    open the target, and check the min_scale
                            ChartBase *ptest_chart = ChartData->OpenChartFromDB( test_db_index,
                                                     FULL_INIT );
                            if( ptest_chart ) min_ref_scale = ptest_chart->GetNormalScaleMin(
                                                                      m_canvas_scale_factor, false );
                        }
                    }
                }

                if( target_stack_index >= 0 ) {
                    new_db_index = m_extended_stack_array.Item( target_stack_index );

                    //  The target chart min/max scales must comply with propsed chart scale on-screen
                    ChartBase *pcandidate = ChartData->OpenChartFromDB( new_db_index, FULL_INIT );
                    double test_max_ref_scale = 1e8;
                    double test_min_ref_scale = 0;
                    if(pcandidate) {
                        test_max_ref_scale = 1.01 * pcandidate->GetNormalScaleMax( m_canvas_scale_factor, cc1->GetCanvasWidth() );
                        test_min_ref_scale = 0.99 * pcandidate->GetNormalScaleMin( m_canvas_scale_factor, false );
                    }

                    if( ( current_family == ChartData->GetDBChartFamily( new_db_index ) )
                            && IsChartQuiltableRef( new_db_index )
                            && (proposed_scale_onscreen >= test_min_ref_scale)
                            && (proposed_scale_onscreen <= test_max_ref_scale) )

                            SetReferenceChart( new_db_index );
                } else {
                    int new_ref_dbIndex = GetNewRefChart();
                    SetReferenceChart( new_ref_dbIndex );
                }
            }
        }
    }
    return m_refchart_dbIndex;
}

bool Quilt::IsChartSmallestScale( int dbIndex )
{
    // find the smallest scale chart of the specified type on the extended stack array

    int specified_type = ChartData->GetDBChartType( dbIndex );
    int target_dbindex = -1;

    unsigned int target_stack_index = 0;
    if( m_extended_stack_array.GetCount() ) {
        while( ( target_stack_index <= ( m_extended_stack_array.GetCount() - 1 ) ) ) {
            int test_db_index = m_extended_stack_array.Item( target_stack_index );

            if( specified_type == ChartData->GetDBChartType( test_db_index ) ) target_dbindex =
                    test_db_index;

            target_stack_index++;
        }
    }
    return ( dbIndex == target_dbindex );
}

wxRegion Quilt::GetHiliteRegion( ViewPort &vp )
{
    wxRegion r;
    if( m_nHiLiteIndex >= 0 ) {
        // Walk the PatchList, looking for the target hilite index
        for( unsigned int i = 0; i < m_PatchList.GetCount(); i++ ) {
            wxPatchListNode *pcinode = m_PatchList.Item( i );
            QuiltPatch *piqp = pcinode->GetData();
            if( ( m_nHiLiteIndex == piqp->dbIndex ) && ( piqp->b_Valid ) ) // found it
            {
                r = piqp->ActiveRegion;
                break;
            }
        }

        // If not in the patchlist, look in the full chartbar
        if( r.IsEmpty() ) {
            for( unsigned int ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
                QuiltCandidate *pqc = m_pcandidate_array->Item( ir );
                if( m_nHiLiteIndex == pqc->dbIndex ) {
                    const ChartTableEntry &cte = ChartData->GetChartTableEntry( m_nHiLiteIndex );
                    wxRegion chart_region = pqc->quilt_region;
                    if( !chart_region.Empty() ) {
                        // Do not highlite fully eclipsed charts
                        bool b_eclipsed = false;
                        for( unsigned int ir = 0; ir < m_eclipsed_stack_array.GetCount(); ir++ ) {
                            if( m_nHiLiteIndex == m_eclipsed_stack_array.Item( ir ) ) {
                                b_eclipsed = true;
                                break;
                            }
                        }

                        if( !b_eclipsed )
                            r = chart_region;
                        break;
                    }
                }
            }
        }
    }
    return r;
}

bool Quilt::BuildExtendedChartStackAndCandidateArray(bool b_fullscreen, int ref_db_index, ViewPort &vp_in)
{
    EmptyCandidateArray();
    m_extended_stack_array.Clear();

    int reference_scale = 1.;
    int reference_type = -1;
    int reference_family;
    int quilt_proj = PROJECTION_UNKNOWN;

    if( ref_db_index >= 0 ) {
        const ChartTableEntry &cte_ref = ChartData->GetChartTableEntry( ref_db_index );
        reference_scale = cte_ref.GetScale();
        reference_type = cte_ref.GetChartType();
        quilt_proj = ChartData->GetDBChartProj( ref_db_index );
        reference_family = cte_ref.GetChartFamily();
    }

    bool b_need_resort = false;

    ViewPort vp_local = vp_in;          // non-const copy

    if( !pCurrentStack ) {
        pCurrentStack = new ChartStack;
        ChartData->BuildChartStack( pCurrentStack, vp_local.clat, vp_local.clon );
    }

    int n_charts = 0;
    if( pCurrentStack ) {
        n_charts = pCurrentStack->nEntry;

        //    Walk the current ChartStack...
        //    Building the quilt candidate array
        for( int ics = 0; ics < n_charts; ics++ ) {
            int i = pCurrentStack->GetDBIndex( ics );
            m_extended_stack_array.Add( i );
            //  A viable candidate?
            double chart_skew = ChartData->GetDBChartSkew( i );
            if( chart_skew > 180. ) chart_skew -= 360.;

            // only unskewed charts of the proper projection and type may be quilted....
            // and we avoid adding CM93 Composite until later
            if( ( reference_type == ChartData->GetDBChartType( i ) )
            && ( fabs( chart_skew ) < 1.0 )
            && ( ChartData->GetDBChartProj( i ) == quilt_proj )
            && ( ChartData->GetDBChartType( i ) != CHART_TYPE_CM93COMP ) ) {
                QuiltCandidate *qcnew = new QuiltCandidate;
                qcnew->dbIndex = i;
                qcnew->ChartScale = ChartData->GetDBChartScale( i );

                //      Calculate and store the quilt region on-screen with the candidate
                const ChartTableEntry &cte = ChartData->GetChartTableEntry( i );
                wxRegion chart_region = GetChartQuiltRegion( cte, vp_local );
                qcnew->quilt_region = chart_region;

                m_pcandidate_array->Add( qcnew );               // auto-sorted on scale

            }
        }
    }

    if( b_fullscreen ) {
        //    Search the entire database, potentially adding all charts
        //    which intersect the ViewPort in any way
        //    .AND. other requirements.
        //    Again, skipping cm93 for now
        int n_all_charts = ChartData->GetChartTableEntries();

        LLBBox viewbox = vp_local.GetBBox();
        int sure_index = -1;
        int sure_index_scale = 0;

        for( int i = 0; i < n_all_charts; i++ ) {
            //    We can eliminate some charts immediately
            //    Try to make these tests in some sensible order....

            if( reference_type != ChartData->GetDBChartType( i ) ) continue;

            if( ChartData->GetDBChartType( i ) == CHART_TYPE_CM93COMP ) continue;

            if( ( g_GroupIndex > 0 ) && ( !ChartData->IsChartInGroup( i, g_GroupIndex ) ) ) continue;

            wxBoundingBox chart_box;
            ChartData->GetDBBoundingBox( i, &chart_box );
            if( ( viewbox.Intersect( chart_box ) == _OUT ) ) continue;

            if( quilt_proj != ChartData->GetDBChartProj( i ) ) continue;

            double chart_skew = ChartData->GetDBChartSkew( i );
            if( chart_skew > 180. ) chart_skew -= 360.;
            if( fabs( chart_skew ) > 1.0 ) continue;

            //    Calculate zoom factor for this chart
            double chart_native_ppm;
            chart_native_ppm = m_canvas_scale_factor / ChartData->GetDBChartScale( i );
            double zoom_factor = vp_in.view_scale_ppm / chart_native_ppm;

            //  Try to guarantee that there is one chart added with scale larger than reference scale
            //    Take note here, and keep track of the smallest scale chart that is larger scale than reference....
            if( ChartData->GetDBChartScale( i ) < reference_scale ) {
                if( ChartData->GetDBChartScale( i ) > sure_index_scale ) {
                    sure_index = i;
                    sure_index_scale = ChartData->GetDBChartScale( i );
                }
            }

            //    At this point, the candidate is the right type, skew, and projection, and is on-screen somewhere....
            //    Now  add the candidate if its scale is smaller than the reference scale, or is not excessively underzoomed.

            if( ( ChartData->GetDBChartScale( i ) >= reference_scale ) || ( zoom_factor > .2 ) ) {
                bool b_add = true;

                const ChartTableEntry &cte = ChartData->GetChartTableEntry( i );
                wxRegion chart_region = GetChartQuiltRegion( cte, vp_local );

                //    Special case for S57 ENC
                //    Add the chart only if the chart's fractional area exceeds n%
                if( CHART_TYPE_S57 == reference_type ) {
                    //Get the fractional area of this chart
                    double chart_fractional_area = 0.;
                    double quilt_area = vp_local.pix_width * vp_local.pix_height;
                    if( !chart_region.Empty() ) {
                        wxRect rect_ch = chart_region.GetBox();
                        chart_fractional_area = ( rect_ch.GetWidth() * rect_ch.GetHeight() )
                                                / quilt_area;
                    } else
                        b_add = false;  // this chart has no overlap on screen
                    // probably because it has a concave outline
                    // i.e the bboxes overlap, but the actual coverage intersect is null.

                    if( chart_fractional_area < .20 ) {
                        b_add = false;
                    }
                    
                    //  Allow S57 charts that are near normal zoom, no matter what their fractional area coverage
                    if( zoom_factor > 0.5)
                        b_add = true;
                }

                if( ref_db_index == i)
                    b_add = true;

                if( b_add ) {
                    // Check to see if this chart is already in the stack array
                    // by virtue of being under the Viewport center point....
                    bool b_exists = false;
                    for( unsigned int ir = 0; ir < m_extended_stack_array.GetCount(); ir++ ) {
                        if( i == m_extended_stack_array.Item( ir ) ) {
                            b_exists = true;
                            break;
                        }
                    }

                    if( !b_exists ) {
                        //      Check to be sure that this chart has not already been added
                        //    i.e. charts that have exactly the same file name and mod time
                        //    These charts can be in the database due to having the exact same chart in different directories,
                        //    as may be desired for some grouping schemes
                        bool b_noadd = false;
                        ChartTableEntry *pn = ChartData->GetpChartTableEntry( i );
                        for( unsigned int id = 0; id < m_extended_stack_array.GetCount() ; id++ ) {
                            if( m_extended_stack_array.Item( id ) != -1 ) {
                                ChartTableEntry *pm = ChartData->GetpChartTableEntry( m_extended_stack_array.Item( id ) );
                                if( pm->GetFileTime() && pn->GetFileTime()) {
                                    if( pm->GetFileTime() == pn->GetFileTime() ) {           // simple test
                                        if( pn->GetpFileName()->IsSameAs( *( pm->GetpFileName() ) ) )
                                            b_noadd = true;
                                    }
                                }
                            }
                        }

                        if(!b_noadd) {
                            m_extended_stack_array.Add( i );

                            QuiltCandidate *qcnew = new QuiltCandidate;
                            qcnew->dbIndex = i;
                            qcnew->ChartScale = ChartData->GetDBChartScale( i );
                            qcnew->quilt_region = chart_region;

                            m_pcandidate_array->Add( qcnew );               // auto-sorted on scale

                            b_need_resort = true;
                        }
                    }
                }
            }
        }               // for all charts

        //    Check to be sure that at least one chart was added that is larger scale than reference scale
        if( -1 != sure_index ) {
            // check to see if it is already in
            bool sure_exists = false;
            for( unsigned int ir = 0; ir < m_extended_stack_array.GetCount(); ir++ ) {
                if( sure_index == m_extended_stack_array.Item(ir) ) {
                    sure_exists = true;
                    break;
                }
            }

            //    If not already added, do so now
            if( !sure_exists ) {
                m_extended_stack_array.Add( sure_index );
                QuiltCandidate *qcnew = new QuiltCandidate;
                qcnew->dbIndex = sure_index;
                qcnew->ChartScale = ChartData->GetDBChartScale( sure_index );
                const ChartTableEntry &cte = ChartData->GetChartTableEntry( sure_index );
                qcnew->quilt_region = GetChartQuiltRegion( cte, vp_local );
                m_pcandidate_array->Add( qcnew );               // auto-sorted on scale

                b_need_resort = true;
            }
        }
    }   // fullscreen

    // Re sort the extended stack array on scale
    if( b_need_resort && m_extended_stack_array.GetCount() > 1 ) {
        int swap = 1;
        int ti;
        while( swap == 1 ) {
            swap = 0;
            for( unsigned int is = 0; is < m_extended_stack_array.GetCount() - 1; is++ ) {
                const ChartTableEntry &m = ChartData->GetChartTableEntry(
                                               m_extended_stack_array.Item( is ) );
                const ChartTableEntry &n = ChartData->GetChartTableEntry(
                                               m_extended_stack_array.Item( is + 1 ) );

                if( n.GetScale() < m.GetScale() ) {
                    ti = m_extended_stack_array.Item( is );
                    m_extended_stack_array.RemoveAt( is );
                    m_extended_stack_array.Insert( ti, is + 1 );
                    swap = 1;
                }
            }
        }
    }
    return true;
}


bool Quilt::Compose( const ViewPort &vp_in )
{
    if( !ChartData ) return false;
    if( m_bbusy ) return false;

    ChartData->UnLockCache();

    ViewPort vp_local = vp_in;                   // need a non-const copy

    //    Get Reference Chart parameters
    if( m_refchart_dbIndex >= 0 ) {
        const ChartTableEntry &cte_ref = ChartData->GetChartTableEntry( m_refchart_dbIndex );
        m_reference_scale = cte_ref.GetScale();
        m_reference_type = cte_ref.GetChartType();
        m_quilt_proj = ChartData->GetDBChartProj( m_refchart_dbIndex );
        m_reference_family = cte_ref.GetChartFamily();
    }

    //    Set up the vieport projection type
    vp_local.SetProjectionType( m_quilt_proj );

    //    As ChartdB data is always in rectilinear space, region calculations need to be done with no VP rotation
    double saved_vp_rotation = vp_local.rotation;                      // save a copy
    vp_local.SetRotationAngle( 0. );
    
    bool bfull = vp_in.b_FullScreenQuilt;
    BuildExtendedChartStackAndCandidateArray(bfull, m_refchart_dbIndex, vp_local);

    //    It is possible that the reference chart is not really part of the visible quilt
    //    This can happen when the reference chart is panned
    //    off-screen in full screen quilt mode
    //    If this situation occurs, we need to immediately select a new reference chart
    //    And rebuild the Candidate Array
    //
    //    A special case occurs with cm93 composite chart set as the reference chart:
    //    It is not at this point a candidate, so won't be found by the search
    //    This case is indicated if the candidate count is zero.
    //    If so, do not invalidate the ref chart
    bool bf = false;
    for( unsigned int i = 0; i < m_pcandidate_array->GetCount(); i++ ) {
        QuiltCandidate *qc = m_pcandidate_array->Item( i );
        if( qc->dbIndex == m_refchart_dbIndex ) {
            bf = true;
            break;
        }
    }

    if( !bf && m_pcandidate_array->GetCount() ) {
        m_refchart_dbIndex = GetNewRefChart();
        BuildExtendedChartStackAndCandidateArray(bfull, m_refchart_dbIndex, vp_local);
    }


    //    Using Region logic, and starting from the largest scale chart
    //    figuratively "draw" charts until the ViewPort window is completely quilted over
    //    Add only those charts whose scale is smaller than the "reference scale"
    wxRegion vp_region( vp_local.rv_rect );
    unsigned int ir;

    //    "Draw" the reference chart first, since it is special in that it controls the fine vpscale setting
    QuiltCandidate *pqc_ref = NULL;
    for( ir = 0; ir < m_pcandidate_array->GetCount(); ir++ )       // find ref chart entry
    {
        QuiltCandidate *pqc = m_pcandidate_array->Item( ir );
        if( pqc->dbIndex == m_refchart_dbIndex ) {
            pqc_ref = pqc;
            break;
        }
    }

    if( pqc_ref ) {
        const ChartTableEntry &cte_ref = ChartData->GetChartTableEntry( m_refchart_dbIndex );

        wxRegion vpu_region( vp_local.rv_rect );

        wxRegion chart_region = pqc_ref->quilt_region; //GetChartQuiltRegion( cte_ref, vp_local );

        if( !chart_region.Empty() ) vpu_region.Intersect( chart_region );

        if( vpu_region.IsEmpty() ) pqc_ref->b_include = false;   // skip this chart, no true overlap
        else {
            pqc_ref->b_include = true;
            vp_region.Subtract( chart_region );          // adding this chart
        }
    }

    //    Now the rest of the candidates
    //    We always walk the entire list for s57 quilts, to pick up eclipsed overlays
    if( !vp_region.IsEmpty() || ( CHART_TYPE_S57 == m_reference_type ) ) {
        for( ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
            QuiltCandidate *pqc = m_pcandidate_array->Item( ir );

            if( pqc->dbIndex == m_refchart_dbIndex ) continue;               // already did this one

            const ChartTableEntry &cte = ChartData->GetChartTableEntry( pqc->dbIndex );


            if( cte.GetScale() >= m_reference_scale ) {
                //  If this chart appears in the no-show array, then simply include it, but
                //  don't subtract its region when determining the smaller scale charts to include.....
                bool b_in_noshow = false;
                for( unsigned int ins = 0; ins < g_quilt_noshow_index_array.GetCount(); ins++ ) {
                    if( g_quilt_noshow_index_array.Item( ins ) == pqc->dbIndex ) // chart is in the noshow list
                    {
                        b_in_noshow = true;
                        break;
                    }
                }

                if( !b_in_noshow ) {
                    //    Check intersection
                    wxRegion vpu_region( vp_local.rv_rect );

                    wxRegion chart_region = pqc->quilt_region;
                    if( !chart_region.Empty() )
                        vpu_region.Intersect( chart_region );

                    if( vpu_region.IsEmpty() )
                        pqc->b_include = false; // skip this chart, no true overlap
                    else {
                        pqc->b_include = true;
                        vp_region.Subtract( chart_region );          // adding this chart
                    }
                } else {
                    pqc->b_include = true;
                }

            } else {
                pqc->b_include = false;                       // skip this chart, scale is too large
            }

            /// Don't break early if the quilt is S57 ENC
            /// This will allow the overlay cells found in Euro(Austrian) IENC to be included
            if( CHART_TYPE_S57 != m_reference_type ) {
                if( vp_region.IsEmpty() )                   // normal stop condition, quilt is full
                    break;
            }
        }
    }

    //    Walk the candidate list again, marking "eclipsed" charts
    //    which at this point are the ones with b_include == false .AND. whose scale is strictly smaller than the ref scale
    //    Also, maintain the member list of same

    m_eclipsed_stack_array.Clear();

    for( ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
        QuiltCandidate *pqc = m_pcandidate_array->Item( ir );

        if( !pqc->b_include ) {
            const ChartTableEntry &cte = ChartData->GetChartTableEntry( pqc->dbIndex );
            if( cte.GetScale() >= m_reference_scale ) {
                m_eclipsed_stack_array.Add( pqc->dbIndex );
                pqc->b_eclipsed = true;
            }
        }
    }

    //    Potentially add cm93 to the candidate array if the region is not yet fully covered
    if( ( m_quilt_proj == PROJECTION_MERCATOR ) && !vp_region.IsEmpty() ) {
        //    Check the remaining unpainted region.
        //    It may contain very small "slivers" of empty space, due to mixing of very small scale charts
        //    with the quilt.  If this is the case, do not waste time loading cm93....

        bool b_must_add_cm93 = false;
        wxRegionIterator updd( vp_region );
        while( updd ) {
            wxRect rect = updd.GetRect();
            if( ( rect.width > 2 ) && ( rect.height > 2 ) ) {
                b_must_add_cm93 = true;
                break;
            }
            updd++;
        }

        if( b_must_add_cm93 ) {
            for( int ics = 0; ics < pCurrentStack->nEntry; ics++ ) {
                int i = pCurrentStack->GetDBIndex( ics );
                if( CHART_TYPE_CM93COMP == ChartData->GetDBChartType( i ) ) {
                    QuiltCandidate *qcnew = new QuiltCandidate;
                    qcnew->dbIndex = i;
                    qcnew->ChartScale = ChartData->GetDBChartScale( i );

                    const ChartTableEntry &cte = ChartData->GetChartTableEntry( i );
                    wxRegion chart_region = GetChartQuiltRegion( cte, vp_local );
                    qcnew->quilt_region = chart_region;

                    m_pcandidate_array->Add( qcnew );
                }
            }
        }
    }

    //    Check the list...if no charts are visible due to all being smaller than reference_scale,
    //    then make sure the smallest scale chart which has any true region intersection is visible anyway
    //    Also enable any other charts which are the same scale as the first one added
    bool b_vis = false;
    for( unsigned int i = 0; i < m_pcandidate_array->GetCount(); i++ ) {
        QuiltCandidate *pqc = m_pcandidate_array->Item( i );
        if( pqc->b_include ) {
            b_vis = true;
            break;
        }
    }

    if( !b_vis && m_pcandidate_array->GetCount() ) {
        int add_scale = 0;

        for( int i = m_pcandidate_array->GetCount() - 1; i >= 0; i-- ) {
            QuiltCandidate *pqc = m_pcandidate_array->Item( i );
            const ChartTableEntry &cte = ChartData->GetChartTableEntry( pqc->dbIndex );

            //    Don't add cm93 yet, it is always covering the quilt...
            if( cte.GetChartType() == CHART_TYPE_CM93COMP ) continue;

            //    Check intersection
            wxRegion vpck_region( vp_local.rv_rect );

            wxRegion chart_region = pqc->quilt_region; //GetChartQuiltRegion( cte, vp_local );
            if( !chart_region.Empty() ) vpck_region.Intersect( chart_region );

            if( !vpck_region.IsEmpty() ) {
                if( add_scale ) {
                    if( add_scale == cte.GetScale() ) pqc->b_include = true;
                    ;
                } else {
                    pqc->b_include = true;
                    add_scale = cte.GetScale();
                }
            }
        }
    }


    //    Finally, build a list of "patches" for the quilt.
    //    Smallest scale first, as this will be the natural drawing order

    m_PatchList.DeleteContents( true );
    m_PatchList.Clear();

    if( m_pcandidate_array->GetCount() ) {
        for( int i = m_pcandidate_array->GetCount() - 1; i >= 0; i-- ) {
            QuiltCandidate *pqc = m_pcandidate_array->Item( i );

            //    cm93 add has been deferred until here
            //    so that it would not displace possible raster or ENCs of larger scale
            const ChartTableEntry &m = ChartData->GetChartTableEntry( pqc->dbIndex );

            if( m.GetChartType() == CHART_TYPE_CM93COMP ) pqc->b_include = true; // force acceptance of this chart in quilt
            // would not be in candidate array if not elected

            if( pqc->b_include ) {
                QuiltPatch *pqp = new QuiltPatch;
                pqp->dbIndex = pqc->dbIndex;
                pqp->ProjType = m.GetChartProjectionType();
                pqp->quilt_region = pqc->quilt_region;
                pqp->b_Valid = true;

                m_PatchList.Append( pqp );
            }
        }
    }
    //    From here on out, the PatchList is usable...

#ifdef QUILT_TYPE_1
    //    Establish the quilt projection type
    m_quilt_proj = PROJECTION_MERCATOR;// default
    ChartBase *ppc = GetLargestScaleChart();
    if(ppc)
        m_quilt_proj = ppc->GetChartProjectionType();
#endif

    //    Walk the PatchList, marking any entries whose projection does not match the determined quilt projection
    for( unsigned int i = 0; i < m_PatchList.GetCount(); i++ ) {
        wxPatchListNode *pcinode = m_PatchList.Item( i );
        QuiltPatch *piqp = pcinode->GetData();
        if( ( piqp->ProjType != m_quilt_proj ) && ( piqp->ProjType != PROJECTION_UNKNOWN ) ) piqp->b_Valid =
                false;
    }

    //    Walk the PatchList, marking any entries which appear in the noshow array
    for( unsigned int i = 0; i < m_PatchList.GetCount(); i++ ) {
        wxPatchListNode *pcinode = m_PatchList.Item( i );
        QuiltPatch *piqp = pcinode->GetData();
        for( unsigned int ins = 0; ins < g_quilt_noshow_index_array.GetCount(); ins++ ) {
            if( g_quilt_noshow_index_array.Item( ins ) == piqp->dbIndex ) // chart is in the noshow list
            {
                piqp->b_Valid = false;
                break;
            }
        }
    }

    //    Generate the final render regions for the patches, one by one, smallest to largest scale
    wxRegion unrendered_region( vp_local.rv_rect );

    m_covered_region.Clear();

    for( unsigned int i = 0; i < m_PatchList.GetCount(); i++ ) {
        wxPatchListNode *pcinode = m_PatchList.Item( i );
        QuiltPatch *piqp = pcinode->GetData();

        if( !piqp->b_Valid )                         // skip invalid entries
            continue;

        const ChartTableEntry &ctei = ChartData->GetChartTableEntry( piqp->dbIndex );

        wxRegion vpr_region = unrendered_region;

        //    Start with the chart's full region coverage.
        vpr_region = piqp->quilt_region;


#if 1       // This clause went away with full-screen quilting
        // ...and came back with OpenGL....

        //fetch and subtract regions for all larger scale charts
        for( unsigned int k = i + 1; k < m_PatchList.GetCount(); k++ ) {
            wxPatchListNode *pnode = m_PatchList.Item( k );
            QuiltPatch *pqp = pnode->GetData();

            if( !pqp->b_Valid )                         // skip invalid entries
                continue;

/// In S57ENC quilts, do not subtract larger scale regions from smaller.
/// This does two things:
/// 1. This allows co-incident or overlayed chart regions to both be included
///    thus covering the case found in layered Euro(Austrian) IENC cells
/// 2. This make quilted S57 ENC renders much faster, as the larger scale charts are not rendered
///     until the canvas is zoomed sufficiently.

/// Above logic does not apply to cm93 composites
            if( ( CHART_TYPE_S57 != ctei.GetChartType() )/* && (CHART_TYPE_CM93COMP != ctei.GetChartType())*/) {

                if( !vpr_region.Empty() ) {
                    const ChartTableEntry &cte = ChartData->GetChartTableEntry( pqp->dbIndex );
                    wxRegion larger_scale_chart_region = pqp->quilt_region; //GetChartQuiltRegion( cte, vp_local );

                    vpr_region.Subtract( larger_scale_chart_region );
                }
            }

        }
#endif

        //    Whatever is left in the vpr region and has not been yet rendered must belong to the current target chart

        wxPatchListNode *pinode = m_PatchList.Item( i );
        QuiltPatch *pqpi = pinode->GetData();
        pqpi->ActiveRegion = vpr_region;

        //    Move the active region so that upper left is 0,0 in final render region
        pqpi->ActiveRegion.Offset( -vp_local.rv_rect.x, -vp_local.rv_rect.y );

        //    Could happen that a larger scale chart covers completely a smaller scale chart
        if( pqpi->ActiveRegion.IsEmpty() )
            pqpi->b_eclipsed = true;

        //    Update the next pass full region to remove the region just allocated
        if( !vpr_region.Empty() )
            unrendered_region.Subtract( vpr_region );

        //    Maintain the present full quilt coverage region
        if( !pqpi->ActiveRegion.IsEmpty() )
            m_covered_region.Union( pqpi->ActiveRegion );
    }

    //    Restore temporary VP Rotation
    vp_local.SetRotationAngle( saved_vp_rotation );

    //    Walk the list again, removing any entries marked as eclipsed....
    unsigned int il = 0;
    while( il < m_PatchList.GetCount() ) {
        wxPatchListNode *pcinode = m_PatchList.Item( il );
        QuiltPatch *piqp = pcinode->GetData();
        if( piqp->b_eclipsed ) {
            //    Make sure that this chart appears in the eclipsed list...
            //    This can happen when....
            bool b_noadd = false;
            for( unsigned int ir = 0; ir < m_eclipsed_stack_array.GetCount(); ir++ ) {
                if( piqp->dbIndex == m_eclipsed_stack_array.Item( ir ) ) {
                    b_noadd = true;
                    break;
                }
            }
            if( !b_noadd ) m_eclipsed_stack_array.Add( piqp->dbIndex );

            m_PatchList.DeleteNode( pcinode );
            il = 0;           // restart the list walk
        }

        else
            il++;
    }

    //    Mark the quilt to indicate need for background clear if the region is not fully covered
    m_bneed_clear = !unrendered_region.IsEmpty();
    m_back_region = unrendered_region;

    cc1->StopAutoPan();
    
    //    Finally, iterate thru the quilt and preload all of the required charts.
    //    For dynamic S57 SENC creation, this is where SENC creation happens first.....
    for( ir = 0; ir < m_pcandidate_array->GetCount(); ir++ ) {
        QuiltCandidate *pqc = m_pcandidate_array->Item( ir );
        if( ( pqc->b_include ) && ( !pqc->b_eclipsed ) ) ChartData->OpenChartFromDB( pqc->dbIndex,
                    FULL_INIT );
    }
    cc1->StopAutoPan();
    
    //    Build and maintain the array of indexes in this quilt

    m_last_index_array = m_index_array;       //save the last one for delta checks

    m_index_array.Clear();

    //    The index array is to be built in reverse, largest scale first
    unsigned int kl = m_PatchList.GetCount();
    for( unsigned int k = 0; k < kl; k++ ) {
        wxPatchListNode *cnode = m_PatchList.Item( ( kl - k ) - 1 );
        m_index_array.Add( cnode->GetData()->dbIndex );
        cnode = cnode->GetNext();
    }

    //    Walk the patch list again, checking the depth units
    //    If they are all the same, then the value is usable

    m_quilt_depth_unit = _T("");
    ChartBase *pc = ChartData->OpenChartFromDB( m_refchart_dbIndex, FULL_INIT );
    if( pc ) {
        m_quilt_depth_unit = pc->GetDepthUnits();

#ifdef USE_S57
        if( pc->GetChartFamily() == CHART_FAMILY_VECTOR ) {
            int units = ps52plib->m_nDepthUnitDisplay;
            switch( units ) {
            case 0:
                m_quilt_depth_unit = _T("Feet");
                break;
            case 1:
                m_quilt_depth_unit = _T("Meters");
                break;
            case 2:
                m_quilt_depth_unit = _T("Fathoms");
                break;
            }
        }
#endif
    }

    for( unsigned int k = 0; k < m_PatchList.GetCount(); k++ ) {
        wxPatchListNode *pnode = m_PatchList.Item( k );
        QuiltPatch *pqp = pnode->GetData();

        if( !pqp->b_Valid )                         // skip invalid entries
            continue;

        ChartBase *pc = ChartData->OpenChartFromDB( pqp->dbIndex, FULL_INIT );
        if( pc ) {
            wxString du = pc->GetDepthUnits();
#ifdef USE_S57
            if( pc->GetChartFamily() == CHART_FAMILY_VECTOR ) {
                int units = ps52plib->m_nDepthUnitDisplay;
                switch( units ) {
                case 0:
                    du = _T("Feet");
                    break;
                case 1:
                    du = _T("Meters");
                    break;
                case 2:
                    du = _T("Fathoms");
                    break;
                }
            }
#endif
            wxString dul = du.Lower();
            wxString ml = m_quilt_depth_unit.Lower();

            if( dul != ml ) {
                //    Try all the odd cases
                if( dul.StartsWith( _T("meters") ) && ml.StartsWith( _T("meters") ) ) continue;
                else if( dul.StartsWith( _T("metres") ) && ml.StartsWith( _T("metres") ) ) continue;
                else if( dul.StartsWith( _T("fathoms") ) && ml.StartsWith( _T("fathoms") ) ) continue;
                else if( dul.StartsWith( _T("met") ) && ml.StartsWith( _T("met") ) ) continue;

                //    They really are different
                m_quilt_depth_unit = _T("");
                break;
            }
        }
    }

    //    And try to prove that all required charts are in the cache
    //    If one is missing, try to load it
    //    If still missing, remove its patch from the quilt
    //    This will probably leave a "black hole" in the quilt...
    for( unsigned int k = 0; k < m_PatchList.GetCount(); k++ ) {
        wxPatchListNode *pnode = m_PatchList.Item( k );
        QuiltPatch *pqp = pnode->GetData();

        if( pqp->b_Valid ) {
            if( !ChartData->IsChartInCache( pqp->dbIndex ) ) {
                wxLogMessage( _T("   Quilt Compose cache miss...") );
                ChartData->OpenChartFromDB( pqp->dbIndex, FULL_INIT );
                if( !ChartData->IsChartInCache( pqp->dbIndex ) ) {
                    wxLogMessage( _T("    Oops, removing from quilt...") );
                    pqp->b_Valid = false;
                }
            }
        }
    }

    //    Make sure the reference chart is in the cache
    if( !ChartData->IsChartInCache( m_refchart_dbIndex ) ) ChartData->OpenChartFromDB(
            m_refchart_dbIndex, FULL_INIT );

    //    Walk the patch list again, checking the error factor
    //    Also, directly mark the patch to indicate if it should be treated as an overlay
    //    as seen in Austrian Inland series

    m_bquilt_has_overlays = false;
    m_max_error_factor = 0.;
    for( unsigned int k = 0; k < m_PatchList.GetCount(); k++ ) {
        wxPatchListNode *pnode = m_PatchList.Item( k );
        QuiltPatch *pqp = pnode->GetData();

        if( !pqp->b_Valid )                         // skip invalid entries
            continue;

        ChartBase *pc = ChartData->OpenChartFromDB( pqp->dbIndex, FULL_INIT );
        if( pc ) {
            m_max_error_factor = wxMax(m_max_error_factor, pc->GetChart_Error_Factor());

            if( pc->GetChartType() == CHART_TYPE_S57 ) {
                s57chart *ps57 = dynamic_cast<s57chart *>( pc );
                pqp->b_overlay = ( ps57->GetUsageChar() == 'L' || ps57->GetUsageChar() == 'A' );
                if( pqp->b_overlay )
                    m_bquilt_has_overlays = true;
            }
        }
    }

    m_bcomposed = true;

    m_vp_quilt = vp_in;                 // save the corresponding ViewPort locally

    ChartData->LockCache();

    //  Create and store a hash value representing the contents of the m_extended_stack_array
    unsigned long xa_hash = 5381;
    for(unsigned int im=0 ; im < m_extended_stack_array.GetCount() ; im++) {
        int dbindex = m_extended_stack_array.Item(im);
        xa_hash = ((xa_hash << 5) + xa_hash) + dbindex; /* hash * 33 + dbindex */
    }

    m_xa_hash = xa_hash;
    return true;
}


//      Compute and update the member quilt render region, considering all scale factors, group exclusions, etc.
void Quilt::ComputeRenderRegion( ViewPort &vp, wxRegion &chart_region )
{
    if( !m_bcomposed ) return;

    wxRegion rendered_region;

    if( GetnCharts() && !m_bbusy ) {

        wxRegion screen_region = chart_region;

        //  Walk the quilt, considering each chart from smallest scale to largest

        ChartBase *chart = GetFirstChart();

        while( chart ) {
            bool okToRender = cc1->IsChartLargeEnoughToRender( chart, vp );

            if( chart->GetChartProjectionType() != PROJECTION_MERCATOR && vp.b_MercatorProjectionOverride )
                okToRender = false;

            if( ! okToRender ) {
                chart = GetNextChart();
                continue;
            }
            QuiltPatch *pqp = GetCurrentPatch();
            if( pqp->b_Valid  ) {
                wxRegion get_region = pqp->ActiveRegion;

                if( !chart_region.IsEmpty() ) {

                    get_region.Intersect( chart_region );

                    if( !get_region.IsEmpty() ) {
                        rendered_region.Union(get_region);
                    }
                }
           }

            chart = GetNextChart();
        }
    }
    //  Record the region actually rendered
    m_rendered_region = rendered_region;
}



int g_render;

bool Quilt::RenderQuiltRegionViewOnDC( wxMemoryDC &dc, ViewPort &vp, wxRegion &chart_region )
{

#ifdef ocpnUSE_DIBSECTION
    ocpnMemDC tmp_dc;
#else
    wxMemoryDC tmp_dc;
#endif

    if( !m_bcomposed ) return false;

    wxRegion rendered_region;

//    double scale_onscreen = vp.view_scale_ppm;
//    double max_allowed_scale = 4. * cc1->GetAbsoluteMinScalePpm();

    if( GetnCharts() && !m_bbusy ) {

        wxRegion screen_region = chart_region;

        //  Walk the quilt, drawing each chart from smallest scale to largest
        //  Render the quilt's charts onto a temp dc
        //  and blit the active region rectangles to to target dc, one-by-one

        ChartBase *chart = GetFirstChart();
        int chartsDrawn = 0;

        if( !chart_region.IsEmpty() ) {
            while( chart ) {
                bool okToRender = cc1->IsChartLargeEnoughToRender( chart, vp );

                if( chart->GetChartProjectionType() != PROJECTION_MERCATOR && vp.b_MercatorProjectionOverride )
                    okToRender = false;

                if( ! okToRender ) {
                    chart = GetNextChart();
                    continue;
                }
                QuiltPatch *pqp = GetCurrentPatch();
                if( pqp->b_Valid  ) {
                    bool b_chart_rendered = false;
                    wxRegion get_region = pqp->ActiveRegion;

                    get_region.Intersect( chart_region );

                    if( !get_region.IsEmpty() ) {

                        if( !pqp->b_overlay ) {
                            b_chart_rendered = chart->RenderRegionViewOnDC( tmp_dc, vp, get_region );
                            if( chart->GetChartType() != CHART_TYPE_CM93COMP )
                                b_chart_rendered = true;
                            screen_region.Subtract( get_region );
                        }
                    }

                    wxRegionIterator upd( get_region );
                    while( upd ) {
                        wxRect rect = upd.GetRect();
                        dc.Blit( rect.x, rect.y, rect.width, rect.height, &tmp_dc, rect.x, rect.y,
                                wxCOPY, true );
                        upd++;
                    }

                    tmp_dc.SelectObject( wxNullBitmap );

                    if(b_chart_rendered)
                        rendered_region.Union(get_region);
                }

                chartsDrawn++;
                chart = GetNextChart();
            }
        }

        if( ! chartsDrawn ) cc1->GetVP().SetProjectionType( PROJECTION_MERCATOR );

        //    Render any Overlay patches for s57 charts(cells)
        if( m_bquilt_has_overlays && !chart_region.IsEmpty() ) {
            chart = GetFirstChart();
            while( chart ) {
                QuiltPatch *pqp = GetCurrentPatch();
                if( pqp->b_Valid ) {
                    if( pqp->b_overlay ) {
                        wxRegion get_region = pqp->ActiveRegion;
                        get_region.Intersect( chart_region );

                        if( !get_region.IsEmpty() ) {
                            s57chart *Chs57 = dynamic_cast<s57chart*>( chart );
                            Chs57->RenderOverlayRegionViewOnDC( tmp_dc, vp, get_region );

                            wxRegionIterator upd( get_region );
                            while( upd ) {
                                wxRect rect = upd.GetRect();
                                dc.Blit( rect.x, rect.y, rect.width, rect.height, &tmp_dc, rect.x,
                                      rect.y, wxCOPY, true );
                                upd++;
                            }
                            tmp_dc.SelectObject( wxNullBitmap );
                        }
                     }
                }

                chart = GetNextChart();
            }
        }

        //    Any part of the chart region that was not rendered in the loop needs to be cleared
        wxRegionIterator clrit( screen_region );
        while( clrit ) {
            wxRect rect = clrit.GetRect();
#ifdef __WXOSX__
            dc.SetPen(*wxBLACK_PEN);
            dc.SetBrush(*wxBLACK_BRUSH);
            dc.DrawRectangle(rect.x, rect.y, rect.width, rect.height);
#else
            dc.Blit( rect.x, rect.y, rect.width, rect.height, &dc, rect.x, rect.y, wxCLEAR );
#endif
            clrit++;
        }

        //    Highlighting....
        if( m_nHiLiteIndex >= 0 ) {
            wxRegion hiregion = GetHiliteRegion( vp );

            wxRect box = hiregion.GetBox();

            if( !box.IsEmpty() ) {
                //    Is scratch member bitmap OK?
                if( m_pBM ) {
                    if( ( m_pBM->GetWidth() != vp.rv_rect.width )
                            || ( m_pBM->GetHeight() != vp.rv_rect.height ) ) {
                        delete m_pBM;
                        m_pBM = NULL;
                    }
                }

                if( NULL == m_pBM ) m_pBM = new wxBitmap( vp.rv_rect.width, vp.rv_rect.height );

                //    Copy the entire quilt to my scratch bm
                wxMemoryDC q_dc;
                q_dc.SelectObject( *m_pBM );
                q_dc.Blit( 0, 0, vp.rv_rect.width, vp.rv_rect.height, &dc, 0, 0 );
                q_dc.SelectObject( wxNullBitmap );

                //    Create a "mask" bitmap from the chart's region
                //    WxGTK has an error in this method....Creates a color bitmap, not usable for mask creation
                //    So, I clone with correction
                wxBitmap hl_mask_bm( vp.rv_rect.width, vp.rv_rect.height, 1 );
                wxMemoryDC mdc;
                mdc.SelectObject( hl_mask_bm );
                mdc.SetBackground( *wxBLACK_BRUSH );
                mdc.Clear();
                mdc.SetClippingRegion( box );
                mdc.SetBackground( *wxWHITE_BRUSH );
                mdc.Clear();
                mdc.SelectObject( wxNullBitmap );

                if( hl_mask_bm.IsOk() ) {
                    wxMask *phl_mask = new wxMask( hl_mask_bm );
                    m_pBM->SetMask( phl_mask );
                    q_dc.SelectObject( *m_pBM );

                    // Create another mask, dc and bitmap for red-out
                    wxBitmap rbm( vp.rv_rect.width, vp.rv_rect.height );
                    wxMask *pr_mask = new wxMask( hl_mask_bm );
                    wxMemoryDC rdc;
                    rbm.SetMask( pr_mask );
                    rdc.SelectObject( rbm );
                    unsigned char hlcolor = 255;
                    switch( global_color_scheme ) {
                    case GLOBAL_COLOR_SCHEME_DAY:
                        hlcolor = 255;
                        break;
                    case GLOBAL_COLOR_SCHEME_DUSK:
                        hlcolor = 64;
                        break;
                    case GLOBAL_COLOR_SCHEME_NIGHT:
                        hlcolor = 16;
                        break;
                    default:
                        hlcolor = 255;
                        break;
                    }

                    rdc.SetBackground( wxBrush( wxColour( hlcolor, 0, 0 ) ) );
                    rdc.Clear();

                    wxRegionIterator upd ( hiregion );
                    while ( upd )
                    {
                        wxRect rect = upd.GetRect();
                        rdc.Blit( rect.x, rect.y, rect.width, rect.height, &q_dc, rect.x, rect.y, wxOR,
                                  true );
                        upd ++ ;
                    }

                    wxRegionIterator updq ( hiregion );
                    while ( updq )
                    {
                        wxRect rect = updq.GetRect();
                        q_dc.Blit( rect.x, rect.y, rect.width, rect.height, &rdc, rect.x, rect.y, wxCOPY,
                                   true );
                        updq ++ ;
                    }


                    q_dc.SelectObject( wxNullBitmap );
                    m_pBM->SetMask( NULL );

                    //    Select the scratch BM as the return dc contents
                    dc.SelectObject( *m_pBM );

                    //    Clear the rdc
                    rdc.SelectObject( wxNullBitmap );
                }
            }  // box not empty
        }     // m_nHiLiteIndex

        if( !dc.IsOk() )          // some error, probably bad charts, to be disabled on next compose
        {
            SubstituteClearDC( dc, vp );
        }

    } else {             // no charts yet, or busy....
        SubstituteClearDC( dc, vp );
    }

    //  Record the region actually rendered
    m_rendered_region = rendered_region;

    m_vp_rendered = vp;
    return true;
}

void Quilt::SubstituteClearDC( wxMemoryDC &dc, ViewPort &vp )
{
    if( m_pBM ) {
        if( ( m_pBM->GetWidth() != vp.rv_rect.width )
                || ( m_pBM->GetHeight() != vp.rv_rect.height ) ) {
            delete m_pBM;
            m_pBM = NULL;
        }
    }

    if( NULL == m_pBM ) {
        m_pBM = new wxBitmap( vp.rv_rect.width, vp.rv_rect.height );
    }

    dc.SelectObject( wxNullBitmap );
    dc.SelectObject( *m_pBM );
    dc.SetBackground( *wxBLACK_BRUSH );
    dc.Clear();
    m_covered_region.Clear();

}

