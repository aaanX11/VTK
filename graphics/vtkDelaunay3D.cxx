/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkDelaunay3D.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$


Copyright (c) 1993-2001 Ken Martin, Will Schroeder, Bill Lorensen 
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither name of Ken Martin, Will Schroeder, or Bill Lorensen nor the names
   of any contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

 * Modified source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

=========================================================================*/
#include "vtkDelaunay3D.h"
#include "vtkMath.h"
#include "vtkTetra.h"
#include "vtkTriangle.h"
#include "vtkEdgeTable.h"
#include "vtkPolyData.h"
#include "vtkObjectFactory.h"

//-------------------------------------------------------------------------
vtkDelaunay3D* vtkDelaunay3D::New()
{
  // First try to create the object from the vtkObjectFactory
  vtkObject* ret = vtkObjectFactory::CreateInstance("vtkDelaunay3D");
  if(ret)
    {
    return (vtkDelaunay3D*)ret;
    }
  // If the factory was unable to create the object, then create it here.
  return new vtkDelaunay3D;
}

//----------------------------------------------------------------------------
// Specify the input data or filter.
void vtkDelaunay3D::SetInput(vtkPointSet *input)
{
  this->vtkProcessObject::SetNthInput(0, input);
}

//----------------------------------------------------------------------------
// Specify the input data or filter.
vtkPointSet *vtkDelaunay3D::GetInput()
{
  if (this->NumberOfInputs < 1)
    {
    return NULL;
    }
  
  return (vtkPointSet *)(this->Inputs[0]);
}

// Structure used to represent sphere around tetrahedron
//
typedef struct _vtkDelaunayTetra 
{
  double r2;
  double center[3];
}
vtkDelaunayTetra;

// Special classes for manipulating tetra array
//
class vtkTetraArray { //;prevent man page generation
public:
  vtkTetraArray(int sz, int extend);
  ~vtkTetraArray()
    {
      if (this->Array)
        {
        delete [] this->Array;
        }
    };
  vtkDelaunayTetra *GetTetra(int tetraId) {return this->Array + tetraId;};
  void InsertTetra(int tetraId, double r2, double center[3]);
  vtkDelaunayTetra *Resize(int sz); //reallocates data

protected:
  vtkDelaunayTetra *Array;  // pointer to data
  int MaxId;              // maximum index inserted thus far
  int Size;               // allocated size of data
  int Extend;             // grow array by this amount
};

vtkTetraArray::vtkTetraArray(int sz, int extend)
{
  this->MaxId = -1; 
  this->Array = new vtkDelaunayTetra[sz];
  this->Size = sz;
  this->Extend = extend;
}

void vtkTetraArray::InsertTetra(int id, double r2, double center[3])
{
  if ( id >= this->Size )
    {
    this->Resize(id+1);
    }
  this->Array[id].r2 = r2;
  this->Array[id].center[0] = center[0];
  this->Array[id].center[1] = center[1];
  this->Array[id].center[2] = center[2];
  if ( id > this->MaxId )
    {
    this->MaxId = id;
    }
}

vtkDelaunayTetra *vtkTetraArray::Resize(int sz)
{
  vtkDelaunayTetra *newArray;
  int newSize;

  if ( sz > this->Size )
    {
    newSize = this->Size + this->Extend*(((sz-this->Size)/this->Extend)+1);
    }
  else if (sz == this->Size)
    {
    return this->Array;
    }
  else
    {
    newSize = sz;
    }

  if ( (newArray = new vtkDelaunayTetra[newSize]) == NULL )
    { 
    vtkGenericWarningMacro(<< "Cannot allocate memory\n");
    return 0;
    }

  if (this->Array)
    {
    memcpy(newArray, this->Array,
           (sz < this->Size ? sz : this->Size) * sizeof(vtkDelaunayTetra));
    delete [] this->Array;
    }

  this->Size = newSize;
  this->Array = newArray;

  return this->Array;
}


// vtkDelaunay3D methods
//

// Construct object with Alpha = 0.0; Tolerance = 0.001; Offset = 2.5;
// BoundingTriangulation turned off.
vtkDelaunay3D::vtkDelaunay3D()
{
  this->NumberOfRequiredInputs = 1;
  this->Alpha = 0.0;
  this->Tolerance = 0.001;
  this->BoundingTriangulation = 0;
  this->Offset = 2.5;
  this->Locator = NULL;
  this->TetraArray = NULL;

  // added for performance
  this->Tetras = vtkIdList::New();
  this->Tetras->Allocate(5);
  this->Faces = vtkIdList::New();
  this->Faces->Allocate(15);
  this->CheckedTetras = vtkIdList::New();
  this->CheckedTetras->Allocate(25);
}

vtkDelaunay3D::~vtkDelaunay3D()
{
  if ( this->Locator )
    {
    this->Locator->UnRegister(this);
    this->Locator = NULL;
    }
  if ( this->TetraArray )
    {
    delete this->TetraArray;
    }
  this->Tetras->Delete();
  this->Faces->Delete();
  this->CheckedTetras->Delete();
}
// special method for performance
static int GetTetraFaceNeighbor(vtkUnstructuredGrid *Mesh, int tetraId, 
                                int p1, int p2, int p3, int& nei);

// Find all faces that enclose a point. (Enclosure means not satifying 
// Delaunay criterion.) This method works in two distinct parts. First, the
// tetrahedra containing the point are found (there may be more than one if 
// the point falls on an edge or face). Next, face neighbors of these points
// are visited to see whether they satisfy the Delaunay criterion. Face 
// neighbors are visited repeatedly until no more tetrahedron are found.
// Enclosing tetras are returned in the tetras list; the enclosing faces 
// are returned in the faces list.
int vtkDelaunay3D::FindEnclosingFaces(float x[3], vtkUnstructuredGrid *Mesh,
                                      vtkIdList *tetras, vtkIdList *faces,
                                      vtkPointLocator *locator)
{
  int tetraId, i, j, numTetras;
  int p1, p2, p3, insertFace;
  int npts, *tetraPts, nei, hasNei;
  int closestPoint;
  double xd[3]; xd[0]=x[0]; xd[1]=x[1]; xd[2]=x[2];

  // Start off by finding closest point and tetras that use the point.
  // This will serve as the starting point to determine an enclosing
  // tetrahedron. (We just need a starting point
  if ( locator->IsInsertedPoint(x) >= 0 ) 
    {
    this->NumberOfDuplicatePoints++;
    return 0;
    }

  closestPoint = locator->FindClosestInsertedPoint(x);
  vtkCellLinks *links = Mesh->GetCellLinks();
  int numCells = links->GetNcells(closestPoint);
  int *cells = links->GetCells(closestPoint);
  if ( numCells < 0 ) //shouldn't happen
    {
    this->NumberOfDegeneracies++;
    return 0;
    }
  else
    {
    tetraId = cells[0];
    }
    
  // Okay, walk towards the containing tetrahedron
  tetraId = this->FindTetra(Mesh,xd,tetraId,0);
  if ( tetraId < 0 ) 
    {
    this->NumberOfDegeneracies++;
    return 0;
    }

  // Initialize the list of tetras who contain the point according
  // to the Delaunay criterion.
  tetras->InsertNextId(tetraId); //means that point is in this tetra

  // Okay, check neighbors for Delaunay criterion. Purpose is to find 
  // list of enclosing faces and deleted tetras.
  numTetras = tetras->GetNumberOfIds();
  for (this->CheckedTetras->Reset(), i=0; i < numTetras; i++) 
    {
    this->CheckedTetras->InsertId(i,tetras->GetId(i));
    }

  for (i=0; i < numTetras; i++)
    {
    tetraId = tetras->GetId(i);
    Mesh->GetCellPoints(tetraId,npts,tetraPts);
    for (j=0; j < 4; j++)
      {
      insertFace = 0;
      p1 = tetraPts[j];
      p2 = tetraPts[(j+1)%4];
      p3 = tetraPts[(j+2)%4];
      hasNei = GetTetraFaceNeighbor(Mesh, tetraId, p1, p2, p3, nei);

      //if a boundary face or an enclosing face
      if ( !hasNei ) //a boundary face
        {
        insertFace = 1;
        }
      else
        {
        if ( this->CheckedTetras->IsId(nei) == -1 ) //if not checked
          {
          if ( this->InSphere(xd,nei) ) //if point inside circumsphere
            {
            numTetras++;
            tetras->InsertNextId(nei); //delete this tetra
            }
          else
            {
            insertFace = 1; //this is a boundary face
            }
          this->CheckedTetras->InsertNextId(nei); //okay, we've checked it
          }
        else
          {
          if ( tetras->IsId(nei) == -1 ) //if checked but not deleted
            {
            insertFace = 1; //a boundary face
            }
          }
        }

      if ( insertFace )
        {
        faces->InsertNextId(p1);
        faces->InsertNextId(p2);
        faces->InsertNextId(p3);
        }

      }//for each tetra face
    }//for all deleted tetras

  // Okay, let's delete the tetras and prepare the data structure 
  for (i=0; i < tetras->GetNumberOfIds(); i++)
    {
    tetraId = tetras->GetId(i);
    Mesh->GetCellPoints(tetraId, npts, tetraPts);
    for (j=0; j<4; j++)
      {
      this->References[tetraPts[j]]--;
      Mesh->RemoveReferenceToCell(tetraPts[j],tetraId);
      }
    }

  return (faces->GetNumberOfIds() / 3);
}

int vtkDelaunay3D::FindTetra(vtkUnstructuredGrid *Mesh, double x[3], 
                             int tetraId, int depth)
{
  double p[4][3];
  double b[4];
  vtkTetra *tetra;
  int neg = 0;
  int j, numNeg;
  double negValue;
  
  // prevent aimless wandering and death by recursion
  if ( depth > 200 )
    {
    return -1;
    }

  tetra = (vtkTetra *)Mesh->GetCell(tetraId);
  for ( j=0; j < 4; j++ ) //load the points
    {
    tetra->Points->GetPoint(j,p[j]);
    }

  vtkTetra::BarycentricCoords(x, p[0], p[1], p[2], p[3], b);

  // find the most negative face
  for ( negValue=VTK_LARGE_FLOAT, numNeg=j=0; j<4; j++ )
    {
    if ( b[j] < 0.0 )
      {
      numNeg++;
      if ( b[j] < negValue )
        {
        negValue = b[j];
        neg = j;
        }
      }
    }

  // if no negatives, then inside this tetra
  if ( numNeg <= 0 )
    {
    return tetraId;
    }
  
  // okay, march towards the most negative direction
  int p1 = 0, p2 = 0, p3 = 0;
  switch (neg) 
    {
    case 0:
      p1 = tetra->PointIds->GetId(1);
      p2 = tetra->PointIds->GetId(2);
      p3 = tetra->PointIds->GetId(3);
      break;
    case 1:
      p1 = tetra->PointIds->GetId(0);
      p2 = tetra->PointIds->GetId(2);
      p3 = tetra->PointIds->GetId(3);
      break;
    case 2:
      p1 = tetra->PointIds->GetId(0);
      p2 = tetra->PointIds->GetId(1);
      p3 = tetra->PointIds->GetId(3);
      break;
    case 3:
      p1 = tetra->PointIds->GetId(0);
      p2 = tetra->PointIds->GetId(1);
      p3 = tetra->PointIds->GetId(2);
      break;
    }
  int nei;
  if ( GetTetraFaceNeighbor(Mesh, tetraId, p1, p2, p3, nei) )
    {
    return this->FindTetra(Mesh, x, nei, ++depth);
    }
  else
    {
    return -1;
    }
}


// 3D Delaunay triangulation. Steps are as follows:
//   1. For each point
//   2. Find tetrahedron point is in
//   3. Repeatedly visit face neighbors and evaluate Delaunay criterion 
//   4. Gather list of faces forming boundary of insertion polyhedron
//   5. Make sure that faces/point combination forms good tetrahedron
//   6. Create tetrahedron from each point/face combination
// 
void vtkDelaunay3D::Execute()
{
  int numPoints, numTetras, i;
  int ptId;
  vtkPoints *inPoints;
  vtkPoints *points;
  vtkUnstructuredGrid *Mesh;
  vtkPointSet *input=this->GetInput();
  vtkUnstructuredGrid *output=this->GetOutput();
  float x[3];
  int pts[4], npts, *tetraPts;
  vtkIdList *cells, *holeTetras;
  float center[3], tol;
  char *tetraUse;
  
  vtkDebugMacro(<<"Generating 3D Delaunay triangulation");

  // Initialize; check input
  //
  if ( (inPoints=input->GetPoints()) == NULL )
    {
    vtkErrorMacro("<<Cannot triangulate; no input points");
    return;
    }

  cells = vtkIdList::New();
  cells->Allocate(64);
  holeTetras = vtkIdList::New();
  holeTetras->Allocate(12);

  numPoints = inPoints->GetNumberOfPoints();

  // Create initial bounding triangulation. Have to create bounding points.
  // Initialize mesh structure.
  input->GetCenter(center);
  tol = input->GetLength();
  Mesh = this->InitPointInsertion(center, this->Offset*tol,
                                  numPoints, points);

  // Insert each point into triangulation. Points lying "inside"
  // of tetra cause tetra to be deleted, leaving a void with bounding
  // faces. Combination of point and each face is used to form new 
  // tetrahedra.
  for (ptId=0; ptId < numPoints; ptId++)
    {
    inPoints->GetPoint(ptId,x);

    this->InsertPoint(Mesh, points, ptId, x, holeTetras);

    if ( ! (ptId % 250) ) 
      {
      vtkDebugMacro(<<"point #" << ptId);
      this->UpdateProgress ((float)ptId/numPoints);
      if (this->GetAbortExecute()) 
        {
        break;
        }
      }
    
    }//for all points

  this->EndPointInsertion();

  vtkDebugMacro(<<"Triangulated " << numPoints <<" points, " 
                << this->NumberOfDuplicatePoints << " of which were duplicates");

  if ( this->NumberOfDegeneracies > 0 )
    {
    vtkWarningMacro(<< this->NumberOfDegeneracies 
                    << " degenerate triangles encountered, mesh quality suspect");
    }

  // Send appropriate portions of triangulation to output
  //
  output->Allocate(5*numPoints);
  numTetras = Mesh->GetNumberOfCells();
  tetraUse = new char[numTetras];
  for (i=0; i < numTetras; i++)
    {
    tetraUse[i] = 2; //mark as non-deleted
    }
  for (i=0; i < holeTetras->GetNumberOfIds(); i++)
    {
    tetraUse[holeTetras->GetId(i)] = 0; //mark as deleted
    }

  //if boundary triangulation not desired, delete tetras connected to 
  // boundary points
  if ( ! this->BoundingTriangulation )
    {
    for (ptId=numPoints; ptId < (numPoints+6); ptId++)
      {
      Mesh->GetPointCells(ptId, cells);
      for (i=0; i < cells->GetNumberOfIds(); i++)
        {
        tetraUse[cells->GetId(i)] = 0; //mark as deleted
        }
      }
    }

  // If non-zero alpha value, then figure out which parts of mesh are
  // contained within alpha radius.
  //
  if ( this->Alpha > 0.0 )
    {
    float alpha2 = this->Alpha * this->Alpha;
    vtkEdgeTable *edges;
    char *pointUse = new char[numPoints+6];
    int p1, p2, p3, nei, hasNei, j, k;
    float x1[3], x2[3], x3[3];
    vtkDelaunayTetra *tetra;
    static int edge[6][2] = {{0,1},{1,2},{2,0},{0,3},{1,3},{2,3}};

    edges = vtkEdgeTable::New();
    edges->InitEdgeInsertion(numPoints+6);

    for (ptId=0; ptId < (numPoints+6); ptId++)
      {
      pointUse[ptId] = 0;
      }

    //traverse all tetras, checking against alpha radius
    for (i=0; i < numTetras; i++)
      {
      //check tetras
      if ( tetraUse[i] == 2 ) //if not deleted
        {
        tetra = this->TetraArray->GetTetra(i);
        if ( tetra->r2 > alpha2 )
          {
          tetraUse[i] = 1; //mark as visited and discarded
          }
        else
          {
          Mesh->GetCellPoints(i, npts, tetraPts);
          for (j=0; j<4; j++)
            {
            pointUse[tetraPts[j]] = 1; 
            }
          for (j=0; j<6; j++)
            {
            p1 = tetraPts[edge[j][0]];
            p2 = tetraPts[edge[j][1]];
            if ( edges->IsEdge(p1,p2) == -1 ) 
              {
              edges->InsertEdge(p1,p2);
              }
            }
          }
        }//if non-deleted tetra
      }//for all tetras

    //traverse tetras again, this time examining faces
    //used tetras have already been output, so we look at those that haven't
    for (i=0; i < numTetras; i++)
      {
      if ( tetraUse[i] == 1 ) //if visited and discarded
        {
        Mesh->GetCellPoints(i, npts, tetraPts);
        for (j=0; j < 4; j++)
          {
          p1 = tetraPts[j];
          p2 = tetraPts[(j+1)%4];
          p3 = tetraPts[(j+2)%4];

          //make sure face is okay to create
          if ( this->BoundingTriangulation || 
          (p1 < numPoints && p2 < numPoints && p3 < numPoints) )
            {
            hasNei = GetTetraFaceNeighbor(Mesh, i, p1,p2,p3, nei);

            if ( !hasNei || ( nei > i && tetraUse[nei]!=2 ) )
              {
              double dx1[3], dx2[3], dx3[3], dv1[3], dv2[3], dv3[3], dcenter[3];
              points->GetPoint(p1,x1); dx1[0]=x1[0]; dx1[1]=x1[1]; dx1[2]=x1[2];
              points->GetPoint(p2,x2); dx2[0]=x2[0]; dx2[1]=x2[1]; dx2[2]=x2[2];
              points->GetPoint(p3,x3); dx3[0]=x3[0]; dx3[1]=x3[1]; dx3[2]=x3[2];
              vtkTriangle::ProjectTo2D(dx1,dx2,dx3,dv1,dv2,dv3);
              if ( vtkTriangle::Circumcircle(dv1,dv2,dv3,dcenter) <= alpha2 )
                {
                pts[0] = p1;
                pts[1] = p2;
                pts[2] = p3;
                output->InsertNextCell(VTK_TRIANGLE,3,pts);
                if ( edges->IsEdge(p1,p2) == -1 )
                  {
                  edges->InsertEdge(p1,p2);
                  }
                if ( edges->IsEdge(p2,p3) == -1 )
                  {
                  edges->InsertEdge(p2,p3);
                  }
                if ( edges->IsEdge(p3,p1) == -1 )
                  {
                  edges->InsertEdge(p3,p1);
                  }
                for (k=0; k<3; k++)
                  {
                  pointUse[pts[k]] = 1; 
                  }
                }
              }//if candidate face
            }//if not boundary face or boundary faces requested
          }//if tetra isn't being output
        }//if tetra not output
      }//for all tetras

    //traverse tetras again, this time examining edges
    for (i=0; i < numTetras; i++)
      {
      if ( tetraUse[i] == 1 ) //one means visited and discarded
        {
        Mesh->GetCellPoints(i, npts, tetraPts);

        for (j=0; j < 6; j++)
          {
          p1 = tetraPts[edge[j][0]];
          p2 = tetraPts[edge[j][1]];

          if ((this->BoundingTriangulation || 
               (p1 < numPoints && p2 < numPoints))
          && (edges->IsEdge(p1,p2) == -1) )
            {
            points->GetPoint(p1,x1);
            points->GetPoint(p2,x2);
            if ( (vtkMath::Distance2BetweenPoints(x1,x2)*0.25) <= alpha2 )
              {
              edges->InsertEdge(p1,p2);
              pts[0] = p1;
              pts[1] = p2;
              output->InsertNextCell(VTK_LINE,2,pts);
              pointUse[p1] = 1; pointUse[p2] = 1; 
              }
            }//if edge a candidate
          }//for all edges of tetra
        }//if tetra not output
      }//for all tetras

    //traverse all points, create vertices if none used
    for (ptId=0; ptId<(numPoints+6); ptId++)
      {
      if (!pointUse[ptId] && (ptId < numPoints || this->BoundingTriangulation))
        {
        pts[0] = ptId;
        output->InsertNextCell(VTK_VERTEX,1,pts);
        }
      }

    // update output
    delete [] pointUse;
    edges->Delete();
    }

  // Update output; free up supporting data structures.
  //
  if ( this->BoundingTriangulation )
    {
    output->SetPoints(points);
    }  
  else
    {
    output->SetPoints(inPoints);
    output->GetPointData()->PassData(input->GetPointData());
    }

  for (i=0; i<numTetras; i++)
    {
    if ( tetraUse[i] == 2 )
      {
      Mesh->GetCellPoints(i,npts,tetraPts);
      output->InsertNextCell(VTK_TETRA,4,tetraPts);
      }
    }
  vtkDebugMacro(<<"Generated " << output->GetNumberOfPoints() << " points and "
                << output->GetNumberOfCells() << " tetrahedra");

  delete [] tetraUse;
  cells->Delete();
  holeTetras->Delete();

  Mesh->Delete();

  output->Squeeze();
}

// This is a helper method used with InsertPoint() to create 
// tetrahedronalizations of points. Its purpose is construct an initial
// Delaunay triangulation into which to inject other points. You must
// specify the center of a cubical bounding box and its length, as well
// as the numer of points to insert. The method returns a pointer to
// an unstructured grid. Use this pointer to manipulate the mesh as
// necessary. You must delete (with Delete()) the mesh when done.
// Note: This initialization method places points forming bounding octahedron
// at the end of the Mesh's point list. That is, InsertPoint() assumes that
// you will be inserting points between (0,numPtsToInsert-1).
vtkUnstructuredGrid *vtkDelaunay3D::InitPointInsertion(float center[3], 
                       float length, int numPtsToInsert, vtkPoints* &points)
{
  float x[3], bounds[6];
  int pts[4], tetraId;
  vtkUnstructuredGrid *Mesh=vtkUnstructuredGrid::New();

  this->NumberOfDuplicatePoints = 0;
  this->NumberOfDegeneracies = 0;

  points = vtkPoints::New();
  points->Allocate(numPtsToInsert+6);

  if ( length <= 0.0 )
    {
    length = 1.0;
    }
  bounds[0] = center[0] - length; bounds[1] = center[0] + length; 
  bounds[2] = center[1] - length; bounds[3] = center[1] + length; 
  bounds[4] = center[2] - length; bounds[5] = center[2] + length; 

  if ( this->Locator == NULL )
    {
    this->CreateDefaultLocator();
    }
  this->Locator->InitPointInsertion(points,bounds);

  //create bounding octahedron: 6 points & 4 tetra
  x[0] = center[0] - length;
  x[1] = center[1];
  x[2] = center[2];
  this->Locator->InsertPoint(numPtsToInsert,x);

  x[0] = center[0] + length;
  x[1] = center[1];
  x[2] = center[2];
  this->Locator->InsertPoint(numPtsToInsert+1,x);

  x[0] = center[0];              
  x[1] = center[1] - length;
  x[2] = center[2];
  this->Locator->InsertPoint(numPtsToInsert+2,x);

  x[0] = center[0];              
  x[1] = center[1] + length;
  x[2] = center[2];
  this->Locator->InsertPoint(numPtsToInsert+3,x);

  x[0] = center[0];              
  x[1] = center[1];
  x[2] = center[2] - length;
  this->Locator->InsertPoint(numPtsToInsert+4,x);

  x[0] = center[0];              
  x[1] = center[1];
  x[2] = center[2] + length;
  this->Locator->InsertPoint(numPtsToInsert+5,x);

  Mesh->Allocate(5*numPtsToInsert);

  if (this->TetraArray)
    {
    delete this->TetraArray;
    }
  
  this->TetraArray = new vtkTetraArray(5*numPtsToInsert,numPtsToInsert);

  //create bounding tetras (there are four)
  pts[0] = numPtsToInsert + 4; pts[1] = numPtsToInsert + 5; 
  pts[2] = numPtsToInsert ; pts[3] = numPtsToInsert + 2;
  tetraId = Mesh->InsertNextCell(VTK_TETRA,4,pts);
  this->InsertTetra(Mesh,points,tetraId);

  pts[0] = numPtsToInsert + 4; pts[1] = numPtsToInsert + 5; 
  pts[2] = numPtsToInsert + 2; pts[3] = numPtsToInsert + 1;
  tetraId = Mesh->InsertNextCell(VTK_TETRA,4,pts);
  this->InsertTetra(Mesh,points,tetraId);

  pts[0] = numPtsToInsert + 4; pts[1] = numPtsToInsert + 5; 
  pts[2] = numPtsToInsert + 1; pts[3] = numPtsToInsert + 3;
  tetraId = Mesh->InsertNextCell(VTK_TETRA,4,pts);
  this->InsertTetra(Mesh,points,tetraId);

  pts[0] = numPtsToInsert + 4; pts[1] = numPtsToInsert + 5; 
  pts[2] = numPtsToInsert + 3; pts[3] = numPtsToInsert;
  tetraId = Mesh->InsertNextCell(VTK_TETRA,4,pts);
  this->InsertTetra(Mesh,points,tetraId);

  Mesh->SetPoints(points);
  points->Delete();
  Mesh->BuildLinks();

  // Keep track of change in references to points
  this->References = new int [numPtsToInsert+6];
  memset(this->References, 0, (numPtsToInsert+6)*sizeof(int));

  return Mesh;
}

// This is a helper method used with InsertPoint() to create 
// tetrahedronalizations of points. Its purpose is construct an initial
// Delaunay triangulation into which to inject other points. You must
// specify the number of points you wish to insert, and then define an
// initial Delaunay tetrahedronalization. This is defined by specifying 
// the number of tetrahedra, and a list of points coordinates defining
// the tetra (total of 4*numTetra points). The method returns a pointer 
// to an unstructured grid. Use this pointer to manipulate the mesh as
// necessary. You must delete (with Delete()) the mesh when done.
// Note: The points you insert using InsertPoint() will range from
// (0,numPtsToInsert-1). Make sure that numPtsToInsert is large enough to
// accomodate this.
vtkUnstructuredGrid *vtkDelaunay3D::InitPointInsertion(int numPtsToInsert,  
               int numTetra, vtkPoints *boundingTetraPts, float bounds[6],
               vtkPoints* &points)
{
  int i, j, pts[4], ptNum, tetraId;
  float *x;
  vtkUnstructuredGrid *Mesh=vtkUnstructuredGrid::New();

  this->NumberOfDuplicatePoints = 0;
  this->NumberOfDegeneracies = 0;

  points = vtkPoints::New();
  points->Allocate(numPtsToInsert+numTetra*4); //estimate

  if ( this->Locator == NULL )
    {
    this->CreateDefaultLocator();
    }
  this->Locator->InitPointInsertion(points,bounds);

  Mesh->Allocate(5*numPtsToInsert);
  this->TetraArray = new vtkTetraArray(5*numPtsToInsert,numPtsToInsert);

  for ( ptNum=0, j=0; j<numTetra; j++)
    {
    for ( i=0; i<4; i++)
      {
      x = boundingTetraPts->GetPoint(4*j+i);
      if ( (pts[i]=this->Locator->IsInsertedPoint(x)) < 0 ) 
        {
        pts[i] = numPtsToInsert + ptNum++;
        this->Locator->InsertPoint(pts[i], x);
        }
      }
    tetraId = Mesh->InsertNextCell(VTK_TETRA,4,pts);
    this->InsertTetra(Mesh, points, tetraId);
    }

  Mesh->SetPoints(points);
  points->Delete();
  Mesh->BuildLinks();

  // Keep track of change in references to points
  this->References = new int [points->GetNumberOfPoints()];
  memset(this->References, 0, points->GetNumberOfPoints()*sizeof(int));

  return Mesh;
}
  
// This is a helper method used with InitPointInsertion() to create
// tetrahedronalizations of points. Its purpose is to inject point at
// coordinates specified into tetrahedronalization. The point id is an index
// into the list of points in the mesh structure.  (See
// vtkDelaunay3D::InitPointInsertion() for more information.)  When you have
// completed inserting points, traverse the mesh structure to extract desired
// tetrahedra (or tetra faces and edges). The holeTetras id list lists all the
// tetrahedra that are deleted (invalid) in the mesh structure.
void vtkDelaunay3D::InsertPoint(vtkUnstructuredGrid *Mesh, vtkPoints *points,
                                int ptId, float x[3], vtkIdList *holeTetras)
{
  int numFaces, tetraId, nodes[4], i;
  int tetraNum, numTetras;

  this->Tetras->Reset();
  this->Faces->Reset();

  // Find faces containing point. (Faces are found by deleting
  // one or more tetrahedra "containing" point.) Tetrahedron contain point
  // when they satisfy Delaunay criterion. (More than one tetra may contain
  // a point if the point is on or near an edge or face.) For each face, 
  // create a tetrahedron. (The locator helps speed search of points 
  // in tetras.)
  if ( (numFaces=this->FindEnclosingFaces(x, Mesh, this->Tetras, 
                                          this->Faces, this->Locator)) > 0 )
    {
    this->Locator->InsertPoint(ptId,x); //point is part of mesh now
    numTetras = this->Tetras->GetNumberOfIds();

    // create new tetra for each face
    for (tetraNum=0; tetraNum < numFaces; tetraNum++)
      {
      //define tetrahedron
      nodes[0] = ptId;
      nodes[1] = this->Faces->GetId(3*tetraNum);
      nodes[2] = this->Faces->GetId(3*tetraNum+1);
      nodes[3] = this->Faces->GetId(3*tetraNum+2);

      //either replace previously deleted tetra or create new one
      if ( tetraNum < numTetras )
        {
        tetraId = this->Tetras->GetId(tetraNum);
        Mesh->ReplaceCell(tetraId, 4, nodes);
        }
      else
        {
        tetraId = Mesh->InsertNextCell(VTK_TETRA,4,nodes);
        }
      
      // Update data structures
      for (i=0; i<4; i++)
        {
        if ( this->References[nodes[i]] >= 0 )
          {
          Mesh->ResizeCellList(nodes[i],5);
          this->References[nodes[i]] -= 5;
          }
        this->References[nodes[i]]++;
        Mesh->AddReferenceToCell(nodes[i],tetraId);
        }

      this->InsertTetra(Mesh, points, tetraId);

      }//for each face

    // Sometimes there are more tetras deleted than created. These
    // have to be accounted for because they leave a "hole" in the
    // data structure. Keep track of them here...mark them deleted later.
    for (tetraNum = numFaces; tetraNum < numTetras; tetraNum++ )
      {
      holeTetras->InsertNextId(this->Tetras->GetId(tetraNum));
      }

    }//if enclosing faces found

}


// Specify a spatial locator for merging points. By default, 
// an instance of vtkMergePoints is used.
void vtkDelaunay3D::SetLocator(vtkPointLocator *locator)
{
  if ( this->Locator == locator ) 
    {
    return;
    }  
  if ( this->Locator )
    {
    this->Locator->UnRegister(this);
    this->Locator = NULL;
    }
  if ( locator )
    {
    locator->Register(this);
    }
  
  this->Locator = locator;
  this->Modified();
}

void vtkDelaunay3D::CreateDefaultLocator()
{
  if ( this->Locator == NULL )
    {
    this->Locator = vtkPointLocator::New();
    this->Locator->SetDivisions(25,25,25);
    }
}

// See whether point is in sphere of tetrahedron
int vtkDelaunay3D::InSphere(double x[3], int tetraId)
{
  double dist2;
  vtkDelaunayTetra *tetra = this->TetraArray->GetTetra(tetraId);
  
  // check if inside/outside circumcircle
  dist2 = (x[0] - tetra->center[0]) * (x[0] - tetra->center[0]) + 
          (x[1] - tetra->center[1]) * (x[1] - tetra->center[1]) +
          (x[2] - tetra->center[2]) * (x[2] - tetra->center[2]);

  if ( dist2 < (0.9999999999L * tetra->r2) )
    {
    return 1;
    }
  else
    {
    return 0;
    }
}

// Compute circumsphere and place into array of tetras
void vtkDelaunay3D::InsertTetra(vtkUnstructuredGrid *Mesh, vtkPoints *points,
                                 int tetraId)
{
  float *x1, *x2, *x3, *x4;
  double dx1[3], dx2[3], dx3[3], dx4[3], radius2, center[3];
  int npts, *pts;

  Mesh->GetCellPoints(tetraId, npts, pts);
  x1 = points->GetPoint(pts[0]); dx1[0]=x1[0]; dx1[1]=x1[1]; dx1[2]=x1[2];
  x2 = points->GetPoint(pts[1]); dx2[0]=x2[0]; dx2[1]=x2[1]; dx2[2]=x2[2];
  x3 = points->GetPoint(pts[2]); dx3[0]=x3[0]; dx3[1]=x3[1]; dx3[2]=x3[2];
  x4 = points->GetPoint(pts[3]); dx4[0]=x4[0]; dx4[1]=x4[1]; dx4[2]=x4[2];

  radius2 = vtkTetra::Circumsphere(dx1,dx2,dx3,dx4,center);
  this->TetraArray->InsertTetra(tetraId, radius2, center);
}

void vtkDelaunay3D::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkUnstructuredGridSource::PrintSelf(os,indent);

  os << indent << "Alpha: " << this->Alpha << "\n";
  os << indent << "Tolerance: " << this->Tolerance << "\n";
  os << indent << "Offset: " << this->Offset << "\n";
  os << indent << "Bounding Triangulation: " 
     << (this->BoundingTriangulation ? "On\n" : "Off\n");

  if ( this->Locator )
    {
    os << indent << "Locator: " << this->Locator << "\n";
    }
  else
    {
    os << indent << "Locator: (none)\n";
    }
}

void vtkDelaunay3D::EndPointInsertion()
{
  if (this->References)
    {
    delete [] this->References;
    this->References = NULL;
    }
}

unsigned long int vtkDelaunay3D::GetMTime()
{
  unsigned long mTime=this->vtkUnstructuredGridSource::GetMTime();
  unsigned long time;

  if ( this->Locator != NULL )
    {
    time = this->Locator->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }
  return mTime;
}

static int GetTetraFaceNeighbor(vtkUnstructuredGrid *Mesh, int tetraId, 
                                int p1, int p2, int p3, int& nei)
{
  // gather necessary information
  vtkCellLinks *links = Mesh->GetCellLinks();
  int numCells = links->GetNcells(p1);
  int *cells = links->GetCells(p1);
  int i, npts, *pts;
  
  //perform set operation
  for (i=0; i < numCells; i++)
    {
    if ( cells[i] != tetraId )
      {
      Mesh->GetCellPoints(cells[i],npts,pts);
      if ( (p2 == pts[0] || p2 == pts[1] || p2 == pts[2] || p2 == pts[3]) &&
           (p3 == pts[0] || p3 == pts[1] || p3 == pts[2] || p3 == pts[3]) )
        {
        nei = cells[i];
        break;
        }
      }//if not referring tetra
    }//for all candidate cells

  if ( i < numCells )
    {
    return 1;
    }
  else
    {
    return 0; //there is no neighbor
    }
}

