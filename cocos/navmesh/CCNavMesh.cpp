/****************************************************************************
 Copyright (c) 2015 Chukong Technologies Inc.
 
 http://www.cocos2d-x.org
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#include "navmesh/CCNavMesh.h"
#if CC_USE_NAVMESH

#include "platform/CCFileUtils.h"
#include "renderer/CCRenderer.h"
#include "recast/Detour/DetourCommon.h"

NS_CC_BEGIN

struct TileCacheSetHeader
{
    int magic;
    int version;
    int numTiles;
    dtNavMeshParams meshParams;
    dtTileCacheParams cacheParams;
};

struct TileCacheTileHeader
{
    dtCompressedTileRef tileRef;
    int dataSize;
};

static const int TILECACHESET_MAGIC = 'T' << 24 | 'S' << 16 | 'E' << 8 | 'T'; //'TSET';
static const int TILECACHESET_VERSION = 1;
static const int MAX_AGENTS = 128;

NavMesh* NavMesh::create(const std::string &filePath)
{
    auto ref = new (std::nothrow) NavMesh();
    if (ref && ref->initWithFilePath(filePath))
    {
        ref->autorelease();
        return ref;
    }
    CC_SAFE_DELETE(ref);
    return nullptr;
}

NavMesh::NavMesh()
    : _navMesh(nullptr)
    , _navMeshQuery(nullptr)
    , _crowed(nullptr)
    , _tileCache(nullptr)
    , _allocator(nullptr)
    , _compressor(nullptr)
    , _meshProcess(nullptr)
{

}

NavMesh::~NavMesh()
{
    dtFreeTileCache(_tileCache);
    dtFreeCrowd(_crowed);
    dtFreeNavMesh(_navMesh);
    dtFreeNavMeshQuery(_navMeshQuery);
    CC_SAFE_DELETE(_allocator);
    CC_SAFE_DELETE(_compressor);
    CC_SAFE_DELETE(_meshProcess);
}

bool NavMesh::initWithFilePath(const std::string &filePath)
{
    _filePath = filePath;
    if (!read()) return false;
    return true;
}

bool NavMesh::read()
{
    std::string fullPath = FileUtils::getInstance()->fullPathForFilename(_filePath);
    FILE* fp = fopen(fullPath.c_str(), "rb");
    if (!fp) return false;

    // Read header.
    TileCacheSetHeader header;
    fread(&header, sizeof(TileCacheSetHeader), 1, fp);
    if (header.magic != TILECACHESET_MAGIC)
    {
        fclose(fp);
        return false;
    }
    if (header.version != TILECACHESET_VERSION)
    {
        fclose(fp);
        return false;
    }

    _navMesh = dtAllocNavMesh();
    if (!_navMesh)
    {
        fclose(fp);
        return false;
    }
    dtStatus status = _navMesh->init(&header.meshParams);
    if (dtStatusFailed(status))
    {
        fclose(fp);
        return false;
    }

    _tileCache = dtAllocTileCache();
    if (!_tileCache)
    {
        fclose(fp);
        return false;
    }

    _allocator = new LinearAllocator(32000);
    _compressor = new FastLZCompressor;
    _meshProcess = new MeshProcess;
    status = _tileCache->init(&header.cacheParams, _allocator, _compressor, _meshProcess);

    if (dtStatusFailed(status))
    {
        fclose(fp);
        return false;
    }

    // Read tiles.
    for (int i = 0; i < header.numTiles; ++i)
    {
        TileCacheTileHeader tileHeader;
        fread(&tileHeader, sizeof(tileHeader), 1, fp);
        if (!tileHeader.tileRef || !tileHeader.dataSize)
            break;

        unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
        if (!data) break;
        memset(data, 0, tileHeader.dataSize);
        fread(data, tileHeader.dataSize, 1, fp);

        dtCompressedTileRef tile = 0;
        _tileCache->addTile(data, tileHeader.dataSize, DT_COMPRESSEDTILE_FREE_DATA, &tile);

        if (tile)
            _tileCache->buildNavMeshTile(tile, _navMesh);
    }

    //create crowed
    _crowed = dtAllocCrowd();
    _crowed->init(MAX_AGENTS, header.cacheParams.walkableRadius, _navMesh);

    //create NavMeshQuery
    _navMeshQuery = dtAllocNavMeshQuery();
    _navMeshQuery->init(_navMesh, 2048);

    _agentList.assign(MAX_AGENTS, nullptr);
    _obstacleList.assign(header.cacheParams.maxObstacles, nullptr);
    //duDebugDrawNavMesh(&_debugDraw, *_navMesh, DU_DRAWNAVMESH_OFFMESHCONS);
    return true;
}

void NavMesh::removeNavMeshObstacle(NavMeshObstacle *obstacle)
{
    auto iter = std::find(_obstacleList.begin(), _obstacleList.end(), obstacle);
    if (iter != _obstacleList.end()){
        obstacle->removeFrom(_tileCache);
        obstacle->release();
        _obstacleList[iter - _obstacleList.begin()] = nullptr;
    }
}

void NavMesh::addNavMeshObstacle(NavMeshObstacle *obstacle)
{
    auto iter = std::find(_obstacleList.begin(), _obstacleList.end(), nullptr);
    if (iter != _obstacleList.end()){
        obstacle->addTo(_tileCache);
        obstacle->retain();
        _obstacleList[iter - _obstacleList.begin()] = obstacle;
    }
}

void NavMesh::removeNavMeshAgent(NavMeshAgent *agent)
{
    auto iter = std::find(_agentList.begin(), _agentList.end(), agent);
    if (iter != _agentList.end()){
        agent->removeFrom(_crowed);
        agent->setNavMeshQuery(nullptr);
        agent->release();
        _agentList[iter - _agentList.begin()] = nullptr;
    }
}

void NavMesh::addNavMeshAgent(NavMeshAgent *agent)
{
    auto iter = std::find(_agentList.begin(), _agentList.end(), nullptr);
    if (iter != _agentList.end()){
        agent->addTo(_crowed);
        agent->setNavMeshQuery(_navMeshQuery);
        agent->retain();
        _agentList[iter - _agentList.begin()] = agent;
    }
}

bool NavMesh::isDebugDrawEnabled() const
{
    return _isDebugDrawEnabled;
}

void NavMesh::setDebugDrawEnable(bool enable)
{
    _isDebugDrawEnabled = enable;
}

void NavMesh::debugDraw(Renderer* renderer)
{
    if (_isDebugDrawEnabled){
        _debugDraw.draw(renderer);
    }
}

void NavMesh::update(float dt)
{
    for (auto iter : _agentList){
        iter->preUpdate(dt);
    }

    for (auto iter : _obstacleList){
        iter->preUpdate(dt);
    }

    if (_crowed)
        _crowed->update(dt, nullptr);

    for (auto iter : _agentList){
        iter->postUpdate(dt);
    }

    for (auto iter : _obstacleList){
        iter->postUpdate(dt);
    }
}

void cocos2d::NavMesh::findPath(const Vec3 &start, const Vec3 &end, std::vector<Vec3> &pathPoints)
{
    static const int MAX_POLYS = 256;
    static const int MAX_SMOOTH = 2048;
    int pathIterNum = 0;
    float ext[3];
    ext[0] = 2; ext[1] = 4; ext[2] = 2;
    dtQueryFilter filter;
    dtPolyRef startRef, endRef;
    dtPolyRef polys[MAX_POLYS];
    int npolys = 0;
    _navMeshQuery->findNearestPoly(&start.x, ext, &filter, &startRef, 0);
    _navMeshQuery->findNearestPoly(&end.x, ext, &filter, &endRef, 0);
    _navMeshQuery->findPath(startRef, endRef, &start.x, &end.x, &filter, polys, &npolys, MAX_POLYS);

    if (npolys)
    {
        //// Iterate over the path to find smooth path on the detail mesh surface.
        //dtPolyRef polys[MAX_POLYS];
        //memcpy(polys, polys, sizeof(dtPolyRef)*npolys);
        //int npolys = npolys;

        float iterPos[3], targetPos[3];
        _navMeshQuery->closestPointOnPoly(startRef, &start.x, iterPos, 0);
        _navMeshQuery->closestPointOnPoly(polys[npolys - 1], &end.x, targetPos, 0);

        static const float STEP_SIZE = 0.5f;
        static const float SLOP = 0.01f;

        int nsmoothPath = 0;
        //dtVcopy(&m_smoothPath[m_nsmoothPath * 3], iterPos);
        //m_nsmoothPath++;

        pathPoints.push_back(Vec3(iterPos[0], iterPos[1], iterPos[2]));
        nsmoothPath++;

        // Move towards target a small advancement at a time until target reached or
        // when ran out of memory to store the path.
        while (npolys && nsmoothPath < MAX_SMOOTH)
        {
            // Find location to steer towards.
            float steerPos[3];
            unsigned char steerPosFlag;
            dtPolyRef steerPosRef;

            if (!getSteerTarget(_navMeshQuery, iterPos, targetPos, SLOP,
                polys, npolys, steerPos, steerPosFlag, steerPosRef))
                break;

            bool endOfPath = (steerPosFlag & DT_STRAIGHTPATH_END) ? true : false;
            bool offMeshConnection = (steerPosFlag & DT_STRAIGHTPATH_OFFMESH_CONNECTION) ? true : false;

            // Find movement delta.
            float delta[3], len;
            dtVsub(delta, steerPos, iterPos);
            len = dtMathSqrtf(dtVdot(delta, delta));
            // If the steer target is end of path or off-mesh link, do not move past the location.
            if ((endOfPath || offMeshConnection) && len < STEP_SIZE)
                len = 1;
            else
                len = STEP_SIZE / len;
            float moveTgt[3];
            dtVmad(moveTgt, iterPos, delta, len);

            // Move
            float result[3];
            dtPolyRef visited[16];
            int nvisited = 0;
            _navMeshQuery->moveAlongSurface(polys[0], iterPos, moveTgt, &filter,
                result, visited, &nvisited, 16);

            npolys = fixupCorridor(polys, npolys, MAX_POLYS, visited, nvisited);
            npolys = fixupShortcuts(polys, npolys, _navMeshQuery);

            float h = 0;
            _navMeshQuery->getPolyHeight(polys[0], result, &h);
            result[1] = h;
            dtVcopy(iterPos, result);

            // Handle end of path and off-mesh links when close enough.
            if (endOfPath && inRange(iterPos, steerPos, SLOP, 1.0f))
            {
                // Reached end of path.
                dtVcopy(iterPos, targetPos);
                if (nsmoothPath < MAX_SMOOTH)
                {
                    //dtVcopy(&m_smoothPath[m_nsmoothPath * 3], iterPos);
                    //m_nsmoothPath++;
                    pathPoints.push_back(Vec3(iterPos[0], iterPos[1], iterPos[2]));
                    nsmoothPath++;
                }
                break;
            }
            else if (offMeshConnection && inRange(iterPos, steerPos, SLOP, 1.0f))
            {
                // Reached off-mesh connection.
                float startPos[3], endPos[3];

                // Advance the path up to and over the off-mesh connection.
                dtPolyRef prevRef = 0, polyRef = polys[0];
                int npos = 0;
                while (npos < npolys && polyRef != steerPosRef)
                {
                    prevRef = polyRef;
                    polyRef = polys[npos];
                    npos++;
                }
                for (int i = npos; i < npolys; ++i)
                    polys[i - npos] = polys[i];
                npolys -= npos;

                // Handle the connection.
                dtStatus status = _navMesh->getOffMeshConnectionPolyEndPoints(prevRef, polyRef, startPos, endPos);
                if (dtStatusSucceed(status))
                {
                    if (nsmoothPath < MAX_SMOOTH)
                    {
                        //dtVcopy(&m_smoothPath[m_nsmoothPath * 3], startPos);
                        //m_nsmoothPath++;
                        pathPoints.push_back(Vec3(startPos[0], startPos[1], startPos[2]));
                        nsmoothPath++;
                        // Hack to make the dotted path not visible during off-mesh connection.
                        if (nsmoothPath & 1)
                        {
                            //dtVcopy(&m_smoothPath[m_nsmoothPath * 3], startPos);
                            //m_nsmoothPath++;
                            pathPoints.push_back(Vec3(startPos[0], startPos[1], startPos[2]));
                            nsmoothPath++;
                        }
                    }
                    // Move position at the other side of the off-mesh link.
                    dtVcopy(iterPos, endPos);
                    float eh = 0.0f;
                    _navMeshQuery->getPolyHeight(polys[0], iterPos, &eh);
                    iterPos[1] = eh;
                }
            }

            // Store results.
            if (nsmoothPath < MAX_SMOOTH)
            {
                //dtVcopy(&m_smoothPath[m_nsmoothPath * 3], iterPos);
                //m_nsmoothPath++;

                pathPoints.push_back(Vec3(iterPos[0], iterPos[1], iterPos[2]));
                nsmoothPath++;
            }
        }
    }
}

NS_CC_END

#endif //CC_USE_NAVMESH
