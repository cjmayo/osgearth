/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "SurfaceNode"
#include "GeometryPool"
#include "TileDrawable"

#include <osgEarth/TileKey>
#include <osgEarth/Registry>
#include <osgEarth/Horizon>

#include <osg/CullStack>
#include <osg/Geode>

#include <osg/Geometry>
#include <osg/TriangleFunctor>
#include <osgText/Text>

#include <numeric>

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define LC "[SurfaceNode] "

//..............................................................

namespace
{    
    osg::Geode* makeBBox(const osg::BoundingBox& bbox, const TileKey& key)
    {        
        osg::Geode* geode = new osg::Geode();
        std::string sizeStr = "(empty)";
        float zpos = 0.0f;

        if ( bbox.valid() )
        {
            osg::Geometry* geom = new osg::Geometry();
            geom->setName("bbox");
        
            osg::Vec3Array* v = new osg::Vec3Array();
            for(int i=0; i<8; ++i)
                v->push_back(bbox.corner(i));
            geom->setVertexArray(v);

            osg::DrawElementsUByte* de = new osg::DrawElementsUByte(GL_LINES);

#if 0
            // bottom:
            de->push_back(0); de->push_back(1);
            de->push_back(1); de->push_back(3);
            de->push_back(3); de->push_back(2);
            de->push_back(2); de->push_back(0);
#endif
#if 1
            // top:
            de->push_back(4); de->push_back(5);
            de->push_back(5); de->push_back(7);
            de->push_back(7); de->push_back(6);
            de->push_back(6); de->push_back(4);
#endif
#if 0
            // corners:
            de->push_back(0); de->push_back(4);
            de->push_back(1); de->push_back(5);
            de->push_back(3); de->push_back(7);
            de->push_back(2); de->push_back(6);
#endif
            geom->addPrimitiveSet(de);

            osg::Vec4Array* c= new osg::Vec4Array();
            c->push_back(osg::Vec4(1,0,0,1));
            geom->setColorArray(c);
            geom->setColorBinding(geom->BIND_OVERALL);

            geode->addDrawable(geom);

            sizeStr = Stringify() << key.str() << "\nmax="<<bbox.zMax()<<"\nmin="<<bbox.zMin()<<"\n";
            zpos = bbox.zMax();
        }

        osgText::Text* textDrawable = new osgText::Text();
        textDrawable->setDataVariance(osg::Object::DYNAMIC);
        textDrawable->setText( sizeStr );
        textDrawable->setFont( osgEarth::Registry::instance()->getDefaultFont() );
        textDrawable->setCharacterSizeMode(textDrawable->SCREEN_COORDS);
        textDrawable->setCharacterSize(32.0f);
        textDrawable->setAlignment(textDrawable->CENTER_CENTER);
        textDrawable->setColor(osg::Vec4(1,1,1,1));
        textDrawable->setBackdropColor(osg::Vec4(0,0,0,1));
        textDrawable->setBackdropType(textDrawable->OUTLINE);
        textDrawable->setPosition(osg::Vec3(0,0,zpos));
        textDrawable->setAutoRotateToScreen(true);
        geode->addDrawable(textDrawable);

        geode->getOrCreateStateSet()->setAttributeAndModes(new osg::Program(),0);
        geode->getOrCreateStateSet()->setMode(GL_LIGHTING,0);
        geode->getOrCreateStateSet()->setRenderBinDetails(INT_MAX, "DepthSortedBin");

        return geode;
    }
}

//..............................................................

HorizonTileCuller::HorizonTileCuller(const SpatialReference* srs, 
                                     const osg::Matrix&      local2world)
{
    _horizonProto.setEllipsoid(*srs->getEllipsoid());
    _radiusPolar = srs->getEllipsoid()->getRadiusPolar();
    _radiusEquator = srs->getEllipsoid()->getRadiusEquator();
    _local2world = local2world;
}

void
HorizonTileCuller::set(const osg::BoundingBox& bbox)
{
    // Adjust the horizon ellipsoid based on the minimum Z value of the tile;
    // necessary because a tile that's below the ellipsoid (ocean floor, e.g.)
    // may be visible even if it doesn't pass the horizon-cone test. In such
    // cases we need a more conservative ellipsoid.
    double zMin = bbox.corner(0).z();
    if ( zMin < 0.0 )
    {
        _horizonProto.setEllipsoid( osg::EllipsoidModel(_radiusEquator + zMin, _radiusPolar + zMin) );
    }            

    // consider the uppermost 4 points of the tile-aligned bounding box.
    // (the last four corners of the bbox are the "zmax" corners.)
    for(unsigned i=0; i<4; ++i)
    {
        _points[i] = bbox.corner(4+i) * _local2world;
    }
}

void
HorizonTileCuller::operator()(osg::Node* node, osg::NodeVisitor* nv)
{
    // Clone the horizon object to support multiple cull threads
    // (since we call setEye with the current node visitor eye point)
    Horizon horizon(_horizonProto);

    // Since each terrain tile has an aboslute reference frame, 
    // there is no need to transform the eyepoint:
    osg::Vec3d vpWorld = osg::Vec3d(nv->getViewPoint()) * _local2world;
    horizon.setEye( vpWorld );

    for(unsigned i=0; i<4; ++i)
    {                   
        if ( horizon.isVisible(_points[i]) )
        {
            traverse(node, nv);
            break;
        }
    }
}

//..............................................................


const bool SurfaceNode::_enableDebugNodes = ::getenv("OSGEARTH_MP_DEBUG") != 0L;

SurfaceNode::SurfaceNode(const TileKey&        tilekey,
                         const MapInfo&        mapinfo,
                         const RenderBindings& bindings,
                         TileDrawable*         drawable)
{
    _tileKey = tilekey;

    _drawable = drawable;

    _surfaceGeode = new osg::Geode();
    _surfaceGeode->addDrawable( drawable );
    
    // Create the final node.
    addChild( _surfaceGeode.get() );

    // Establish a local reference frame for the tile:
    GeoPoint centroid;
    tilekey.getExtent().getCentroid(centroid);

    osg::Matrix local2world;
    centroid.createLocalToWorld( local2world );
    setMatrix( local2world );

    //_worldCorners.resize(8);
    //_childrenCorners.resize(4);
    //for(size_t i = 0; i < _childrenCorners.size(); ++i)
    //{
    //    _childrenCorners[i].resize(8);
    //}

    // Initialize the cached bounding box.
    setElevationRaster( 0L, osg::Matrixf::identity() );
}

float
SurfaceNode::minSquaredDistanceFromPoint(const VectorPoints& corners, const osg::Vec3& center, float z2)
{   
    float mind2 = FLT_MAX;
    for(int i=0; i<8; ++i)
    {
        const osg::Vec3& corner = corners[i];
    //for( VectorPoints::const_iterator i=corners.begin(); i != corners.end(); ++i )
    //{
        float d2 = (corner-center).length2() * z2;
        //float d2 = (*i - center).length2()*z2;
        if ( d2 < mind2 ) mind2 = d2;
    }
    return mind2;
}

bool
SurfaceNode::anyChildBoxIntersectsSphere(const osg::Vec3& center, float radiusSquared, float fZoomFactor)
{
    float z2 = fZoomFactor*fZoomFactor;
    for(int i=0; i<4; ++i)
    {
        const VectorPoints& childCorners = _childrenCorners[i];
    //for(ChildrenCorners::const_iterator it = _childrenCorners.begin(); it != _childrenCorners.end(); ++it)
    //{
    //    const VectorPoints& childCorners = *it;
        float fMinDistanceSquared = minSquaredDistanceFromPoint(childCorners, center, z2);
        if (fMinDistanceSquared <= radiusSquared)
        {
            return true;
        }
    }
    return false;
}

void
SurfaceNode::setElevationRaster(const osg::Image*   raster,
                                const osg::Matrixf& scaleBias)
{
    if ( !_drawable.valid() )
        return;

    // communicate the raster to the drawable:
    if ( raster )
    {
        _drawable->setElevationRaster( raster, scaleBias );
    }

    // next compute the bounding box in local space:
#if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
    const osg::BoundingBox& box = _drawable->getBoundingBox();
#else
    const osg::BoundingBox& box = _drawable->getBound();
#endif

    // Compute the medians of each potential child node:

    osg::Vec3 minZMedians[4];
    osg::Vec3 maxZMedians[4];

    minZMedians[0] = (box.corner(0)+box.corner(1))*0.5;
    minZMedians[1] = (box.corner(1)+box.corner(3))*0.5;
    minZMedians[2] = (box.corner(3)+box.corner(2))*0.5;
    minZMedians[3] = (box.corner(0)+box.corner(2))*0.5;
                                  
    maxZMedians[0] = (box.corner(4)+box.corner(5))*0.5;
    maxZMedians[1] = (box.corner(5)+box.corner(7))*0.5;
    maxZMedians[2] = (box.corner(7)+box.corner(6))*0.5;
    maxZMedians[3] = (box.corner(4)+box.corner(6))*0.5;
                                  
    // Child 0 corners
    _childrenCorners[0][0] =  box.corner(0);
    _childrenCorners[0][1] =  minZMedians[0];
    _childrenCorners[0][2] =  minZMedians[3];
    _childrenCorners[0][3] = (minZMedians[0]+minZMedians[2])*0.5;

    _childrenCorners[0][4] =  box.corner(4);
    _childrenCorners[0][5] =  maxZMedians[0];
    _childrenCorners[0][6] =  maxZMedians[3];
    _childrenCorners[0][7] = (maxZMedians[0]+maxZMedians[2])*0.5;

    // Child 1 corners
    _childrenCorners[1][0] =  minZMedians[0];
    _childrenCorners[1][1] =  box.corner(1);
    _childrenCorners[1][2] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[1][3] =  minZMedians[1];
                     
    _childrenCorners[1][4] =  maxZMedians[0];
    _childrenCorners[1][5] =  box.corner(5);
    _childrenCorners[1][6] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[1][7] =  maxZMedians[1];

    // Child 2 corners
    _childrenCorners[2][0] =  minZMedians[3];
    _childrenCorners[2][1] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[2][2] =  box.corner(2);
    _childrenCorners[2][3] =  minZMedians[2];
                     
    _childrenCorners[2][4] =  maxZMedians[3];
    _childrenCorners[2][5] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[2][6] =  box.corner(6);
    _childrenCorners[2][7] =  maxZMedians[2]; 

    // Child 3 corners
    _childrenCorners[3][0] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[3][1] =  minZMedians[1];
    _childrenCorners[3][2] =  minZMedians[2];
    _childrenCorners[3][3] =  box.corner(3);
                     
    _childrenCorners[3][4] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[3][5] =  maxZMedians[1];
    _childrenCorners[3][6] =  maxZMedians[2];
    _childrenCorners[3][7] =  box.corner(7);

    // Transform the child corners to world space
    
    const osg::Matrix& local2world = getMatrix();
    for(int i=0; i<4; ++i)
    {
        VectorPoints& childCorners = _childrenCorners[i];
         for(int j=0; j<8; ++j)
         {
             osg::Vec3& corner = childCorners[j];
             corner = corner*local2world;
         }
    }

    if( _enableDebugNodes )
    {
        removeDebugNode();
        addDebugNode(box);
    }

    // Update the horizon culling callback.
    if ( !_horizonCuller.valid() )
    {
        _horizonCuller = new HorizonTileCuller( _tileKey.getProfile()->getSRS(), getMatrix() );
        setCullCallback( _horizonCuller.get() );
    }

    _horizonCuller->set( box );

    // need this?
    dirtyBound();
}

const osg::Image*
SurfaceNode::getElevationRaster() const
{
    return _drawable->getElevationRaster();
}

const osg::Matrixf&
SurfaceNode::getElevationMatrix() const
{
    return _drawable->getElevationMatrix();
};

void
SurfaceNode::addDebugNode(const osg::BoundingBox& box)
{
    _debugText = 0;
    _debugGeode = makeBBox(box, _tileKey);
    addChild( _debugGeode.get() );
}

void
SurfaceNode::removeDebugNode(void)
{
    _debugText = 0;
    if ( _debugGeode.valid() )
    {
        removeChild( _debugGeode.get() );
    }
}

void
SurfaceNode::setDebugText(const std::string& strText)
{
    if (_debugText.valid()==false)
    {
        return;
    }
    _debugText->setText(strText);
}

const osg::BoundingBox&
SurfaceNode::getAlignedBoundingBox() const
{
#if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
    return _drawable->getBoundingBox();
#else
    return _drawable->getBound();
#endif
}
