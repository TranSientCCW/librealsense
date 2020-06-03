//// License: Apache 2.0. See LICENSE file in root directory.
//// Copyright(c) 2020 Intel Corporation. All Rights Reserved.

#include "optimizer.h"
#include <librealsense2/rsutil.h>
#include <algorithm>
#include <array>
#include "coeffs.h"
#include "cost.h"
#include "uvmap.h"
#include "k-to-dsm.h"
#include "debug.h"
#include "utils.h"

using namespace librealsense::algo::depth_to_rgb_calibration;


namespace
{
    std::vector<double> calc_intensity( std::vector<double> const & image1, std::vector<double> const & image2 )
    {
        std::vector<double> res( image1.size(), 0 );

        for( auto i = 0; i < image1.size(); i++ )
        {
            res[i] = sqrt( pow( image1[i], 2 ) + pow( image2[i], 2 ) );
        }
        return res;
    }

    template<class T>
    double dot_product( std::vector<T> const & sub_image, std::vector<double> const & mask )
    {
        double res = 0;

        for( auto i = 0; i < sub_image.size(); i++ )
        {
            res += sub_image[i] * mask[i];
        }

        return res;
    }

    template<class T>
    std::vector<double> convolution(std::vector<T> const& image,
        size_t image_width, size_t image_height,
        size_t mask_width, size_t mask_height,
        std::function< double(std::vector<T> const& sub_image) > convolution_operation
        )
    {
        std::vector<double> res(image.size(), 0);

        for (auto i = 0; i < image_height - mask_height + 1; i++)
        {
            for (size_t j = 0; j < image_width - mask_width + 1; j++)
            {
                std::vector<T> sub_image(mask_width * mask_height, 0);
                auto ind = 0;
                for (size_t l = 0; l < mask_height; l++)
                {
                    for (size_t k = 0; k < mask_width; k++)
                    {
                        size_t p = (i + l) * image_width + j + k;
                        sub_image[ind++] = (image[p]);
                    }

                }
                auto mid = (i + mask_height / 2) * image_width + j + mask_width / 2;

                res[mid] = convolution_operation(sub_image);

            }
        }
        return res;
    }
   
    template<class T>
    std::vector<double> calc_horizontal_gradient( std::vector<T> const & image, size_t image_width, size_t image_height )
    {
        std::vector<double> horizontal_gradients = { -1, -2, -1,
                                                      0,  0,  0,
                                                      1,  2,  1 };

        return convolution<T>( image, image_width, image_height, 3, 3, [&]( std::vector<T> const & sub_image )
            {return dot_product( sub_image, horizontal_gradients ) / (double)8; } );
    }

    template<class T>
    std::vector<double> calc_vertical_gradient( std::vector<T> const & image, size_t image_width, size_t image_height )
    {
        std::vector<double> vertical_gradients = { -1, 0, 1,
                                                   -2, 0, 2,
                                                   -1, 0, 1 };

        return convolution<T>( image, image_width, image_height, 3, 3, [&]( std::vector<T> const & sub_image )
            {return dot_product( sub_image, vertical_gradients ) / (double)8; } );;
    }

    template<class T>
    std::vector<double> calc_edges( std::vector<T> const & image, size_t image_width, size_t image_height )
    {
        auto vertical_edge = calc_vertical_gradient( image, image_width, image_height );

        auto horizontal_edge = calc_horizontal_gradient( image, image_width, image_height );

        auto edges = calc_intensity( vertical_edge, horizontal_edge );

        return edges;
    }

    std::pair<int, int> const dir_map[direction::deg_none] =
    {
        { 1, 0},  // dir_0
        { 1, 1},  // dir_45
        { 0, 1},  // deg_90
        {-1, 1}   // deg_135
    };

}


optimizer::optimizer()
{
}

static std::vector< double > get_direction_deg(
    std::vector<double> const & gradient_x,
    std::vector<double> const & gradient_y
)
{
    std::vector<double> res( gradient_x.size(), deg_none );

    for( auto i = 0; i < gradient_x.size(); i++ )
    {
        int closest = -1;
        auto angle = atan2( gradient_y[i], gradient_x[i] )* 180.f / M_PI;
        angle = angle < 0 ? 180 + angle : angle;
        auto dir = fmod( angle, 180 );


        res[i] = dir;
    }
    return res;
}
static std::vector< double > get_direction_deg2(
    std::vector<double> const& gradient_x,
    std::vector<double> const& gradient_y
    )
{
    std::vector<double> res(gradient_x.size(), deg_none);

    for (auto i = 0; i < gradient_x.size(); i++)
    {
        int closest = -1;
        auto angle = atan2(gradient_y[i], gradient_x[i])*180.f  / M_PI;
        angle = angle < 0 ?  360+angle : angle;
        auto dir = fmod(angle, 360);


        res[i] = dir;
    }
    return res;
}
static
std::pair< int, int > get_prev_index(
    direction dir,
    int i, int j,
    size_t width, size_t height )
{
    int edge_minus_idx = 0;
    int edge_minus_idy = 0;

    auto & d = dir_map[dir];
    if( j < d.first )
        edge_minus_idx = int(width) - 1 - j;
    else if( j - d.first >= width )
        edge_minus_idx = 0;
    else
        edge_minus_idx = j - d.first;

    if( i - d.second < 0 )
        edge_minus_idy = int(height) - 1 - i;
    else if( i - d.second >= height )
        edge_minus_idy = 0;
    else
        edge_minus_idy = i - d.second;

    return { edge_minus_idx, edge_minus_idy };
}

static
std::pair< int, int > get_next_index(
    direction dir,
    int i, int j,
    size_t width, size_t height
)
{
    auto & d = dir_map[dir];

    int edge_plus_idx = 0;
    int edge_plus_idy = 0;

    if( j + d.first < 0 )
        edge_plus_idx = int(width) - 1 - j;
    else if( j + d.first >= width )
        edge_plus_idx = 0;
    else
        edge_plus_idx = j + d.first;

    if( i + d.second < 0 )
        edge_plus_idy = int(height) - 1 - i;
    else if( i + d.second >= height )
        edge_plus_idy = 0;
    else
        edge_plus_idy = i + d.second;

    return { edge_plus_idx, edge_plus_idy };
}

void set_margin(
    std::vector<double>& gradient,
    double margin,
    size_t width,
    size_t height)
{
    auto it = gradient.begin();
    for (auto i = 0; i < width; i++)
    {
        // zero mask of 2nd row, and row before the last
        *(it + width + i) = 0;
        *(it + width*(height-2) + i) = 0;
    }
    for (auto i = 0; i < height; i++)
    {
        // zero mask of 2nd column, and column before the last
        *(it + i*width+1) = 0;
        *(it + i * width + (width-2)) = 0;
    }
}

template < class T >
void sample_by_mask( std::vector< T > & filtered,
                     std::vector< T > const & origin,
                     std::vector< byte > const & valid_edge_by_ir,
                     size_t const width,
                     size_t const height )
{
    //%function [values] = sampleByMask(I,binMask)
    //%    % Extract values from image I using the binMask with the order being
    //%    % row and then column
    //%    I = I';
    //%    values = I(binMask');
    //end
    for( auto x = 0; x < origin.size(); ++x )
        if( valid_edge_by_ir[x] )
            filtered.push_back( origin[x] );
}

template < class T >
void depth_filter( std::vector< T > & filtered,
                   std::vector< T > const & origin,
                   std::vector< byte > const & valid_edge_by_ir,
                   size_t const width,
                   size_t const height )
{
    // origin and valid_edge_by_ir are of same size
    for( auto j = 0; j < width; j++ )
    {
        for( auto i = 0; i < height; i++ )
        {
            auto idx = i * width + j;
            if( valid_edge_by_ir[idx] )
            {
                filtered.push_back( origin[idx] );
            }
        }
    }
}

void grid_xy(
    std::vector<double>& gridx,
    std::vector<double>& gridy,
    size_t width,
    size_t height)
{
    for (auto i = 1; i <= height; i++)
    {
        for (auto j = 1; j <= width; j++)
        {
            gridx.push_back(j);
            gridy.push_back(i);
        }
    }
}
template<class T>
std::vector< double > interpolation( std::vector< T > const & grid_points,
                                     std::vector< double > const x[], std::vector< double > const y[],
                                     size_t dim, size_t valid_size, size_t valid_width )
{
    // interpolation 

    std::vector<double> local_interp;
    auto iedge_it = grid_points.begin();// iEdge   
    std::vector<double>::const_iterator loc_reg_x[4];
    std::vector<double>::const_iterator loc_reg_y[4];
    for( auto i = 0; i < dim; i++ )
    {
        loc_reg_x[i] = x[i].begin();
        loc_reg_y[i] = y[i].begin();
    }
    for (auto i = 0; i < valid_size; i++)
    {
        for (auto k = 0; k < dim; k++)
        {
            auto idx = *(loc_reg_x[k] + i) - 1;
            auto idy = *(loc_reg_y[k] + i) - 1;
            //assert(_ir.width * idy + idx <= _ir.width * _ir.height);
            auto val = *( iedge_it + size_t( valid_width * idy + idx ) );  // find value in iEdge
            local_interp.push_back(val);
        }
    }
    return local_interp;
}
std::vector<uint8_t> is_suppressed(std::vector<double> const & local_edges, size_t valid_size)
{
    std::vector<uint8_t> is_supressed;
    auto loc_edg_it = local_edges.begin();
    for (auto i = 0; i < valid_size; i++)
    {
        //isSupressed = localEdges(:,3) >= localEdges(:,2) & localEdges(:,3) >= localEdges(:,4);
        auto vec2 = *(loc_edg_it + 1);
        auto vec3 = *(loc_edg_it + 2);
        auto vec4 = *(loc_edg_it + 3);
        loc_edg_it += 4;
        bool res = (vec3 >= vec2) && (vec3 >= vec4);
        is_supressed.push_back(res);
    }
    return is_supressed;
}

std::vector<double> depth_mean(std::vector<double>& local_x, std::vector<double>& local_y)
{
    std::vector<double> res;
    size_t size = local_x.size() / 2;
    auto itx = local_x.begin();
    auto ity = local_y.begin();
    for (auto i = 0; i < size; i++, ity += 2, itx += 2)
    {
        double valy = (*ity + *(ity + 1)) / 2;
        double valx = (*itx + *(itx + 1)) / 2;
        res.push_back(valy);
        res.push_back(valx);
    }

    return res;
}
std::vector<double> sum_gradient_depth(std::vector<double> &gradient, std::vector<double> &direction_per_pixel)
{
    std::vector<double> res;
    size_t size = direction_per_pixel.size() / 2;
    auto it_dir = direction_per_pixel.begin();
    auto it_grad = gradient.begin();
    for (auto i = 0; i < size; i++, it_dir+=2, it_grad+=2)
    {
        // normalize : res = val/sqrt(row_sum)
        auto rorm_dir1 = *it_dir / sqrt(abs(*it_dir) + abs(*(it_dir + 1)));
        auto rorm_dir2 = *(it_dir+1) / sqrt(abs(*it_dir) + abs(*(it_dir + 1)));
        auto val = abs(*it_grad * rorm_dir1 + *(it_grad + 1) * rorm_dir2);
        res.push_back(val);
    }
    return res;
}

std::vector< byte > find_valid_depth_edges( std::vector< double > const & grad_in_direction,
                                            std::vector< byte > const & is_supressed,
                                            std::vector< double > const & values_for_subedges,
                                            int const gradZTh )
{
    std::vector< byte > res;
    res.reserve( grad_in_direction.size() );
    //%validEdgePixels = zGradInDirection > params.gradZTh & isSupressed & zValuesForSubEdges > 0;
    for (int i = 0; i < grad_in_direction.size(); i++)
    {
        bool cond1 = grad_in_direction[i] > gradZTh;
        bool cond2 = is_supressed[i];
        bool cond3 = values_for_subedges[i] > 0;
        res.push_back( cond1 && cond2 && cond3 );
    }
    return res;
}

std::vector<double> find_local_values_min(std::vector<double>& local_values)
{
    std::vector<double> res;
    size_t size = local_values.size() / 4;
    auto it = local_values.begin();
    for (auto i = 0; i < size; i++)
    {
        auto val1 = *it;
        auto val2 = *(it + 1);
        auto val3 = *(it + 2);
        auto val4 = *(it + 3);
        it += 4;
        double res_val = std::min(val1, std::min(val2, std::min(val3, val4)));
        res.push_back(res_val);
    }
    return res;
}
void optimizer::set_z_data( std::vector< z_t > && depth_data,
                            rs2_intrinsics_double const & depth_intrinsics,
                            rs2_dsm_params const & dsm_params,
                            algo_calibration_info const & cal_info,
                            algo_calibration_registers const & cal_regs, float depth_units )
{
    _original_dsm_params = dsm_params;
    _k_to_DSM = std::make_shared<k_to_DSM>(dsm_params, cal_info, cal_regs, _params.max_scaling_step);

    /*[zEdge,Zx,Zy] = OnlineCalibration.aux.edgeSobelXY(uint16(frame.z),2); % Added the second input - margin to zero out
    [iEdge,Ix,Iy] = OnlineCalibration.aux.edgeSobelXY(uint16(frame.i),2); % Added the second input - margin to zero out
    validEdgePixelsByIR = iEdge>params.gradITh; */
    _params.set_depth_resolution(depth_intrinsics.width, depth_intrinsics.height);
    _z.width = depth_intrinsics.width;
    _z.height = depth_intrinsics.height;
    _z.orig_intrinsics = depth_intrinsics;
    _z.orig_dsm_params = dsm_params;
    _z.depth_units = depth_units;

    _z.frame = std::move(depth_data);

    _z.gradient_x = calc_vertical_gradient(_z.frame, depth_intrinsics.width, depth_intrinsics.height);
    _z.gradient_y = calc_horizontal_gradient(_z.frame, depth_intrinsics.width, depth_intrinsics.height);
    _ir.gradient_x = calc_vertical_gradient(_ir.ir_frame, depth_intrinsics.width, depth_intrinsics.height);
    _ir.gradient_y = calc_horizontal_gradient(_ir.ir_frame, depth_intrinsics.width, depth_intrinsics.height);

    // set margin of 2 pixels to 0
    set_margin(_z.gradient_x, 2, _z.width, _z.height);
    set_margin(_z.gradient_y, 2, _z.width, _z.height);
    set_margin(_ir.gradient_x, 2, _z.width, _z.height);
    set_margin(_ir.gradient_y, 2, _z.width, _z.height);

    _z.edges = calc_intensity(_z.gradient_x, _z.gradient_y);
    _ir.edges = calc_intensity(_ir.gradient_x, _ir.gradient_y);

    for( auto it = _ir.edges.begin(); it < _ir.edges.end(); it++ )
        _ir.valid_edge_pixels_by_ir.push_back( *it > _params.grad_ir_threshold );

    /*sz = size(frame.i);
    [gridX,gridY] = meshgrid(1:sz(2),1:sz(1)); % gridX/Y contains the indices of the pixels
    sectionMapDepth = OnlineCalibration.aux.sectionPerPixel(params);
    */
    // Get a map for each pixel to its corresponding section
    _z.section_map_depth.resize(_z.width * _z.height);
    size_t const section_w = _params.num_of_sections_for_edge_distribution_x;  //% params.numSectionsH
    size_t const section_h = _params.num_of_sections_for_edge_distribution_y;  //% params.numSectionsH
    section_per_pixel(_z, section_w, section_h, _z.section_map_depth.data());

    //%locRC = [sampleByMask( gridY, validEdgePixelsByIR ), sampleByMask( gridX, validEdgePixelsByIR )];
    //%sectionMapValid = sampleByMask( sectionMapDepth, validEdgePixelsByIR );
    //%IxValid = sampleByMask( Ix, validEdgePixelsByIR );
    //%IyValid = sampleByMask( Iy, validEdgePixelsByIR );

    std::vector<double> grid_x;
    std::vector<double> grid_y;
    grid_xy(grid_x, grid_y, _z.width, _z.height);

    sample_by_mask( _ir.valid_location_rc_x, grid_x, _ir.valid_edge_pixels_by_ir, _z.width, _z.height );
    sample_by_mask( _ir.valid_location_rc_y, grid_y, _ir.valid_edge_pixels_by_ir, _z.width, _z.height );
    sample_by_mask( _ir.valid_section_map, _z.section_map_depth, _ir.valid_edge_pixels_by_ir, _z.width, _z.height );
    sample_by_mask( _ir.valid_gradient_x, _ir.gradient_x, _ir.valid_edge_pixels_by_ir, _z.width, _z.height );
    sample_by_mask( _ir.valid_gradient_y, _ir.gradient_y, _ir.valid_edge_pixels_by_ir, _z.width, _z.height );

    auto itx = _ir.valid_location_rc_x.begin();
    auto ity = _ir.valid_location_rc_y.begin();
    for (auto i = 0; i < _ir.valid_location_rc_x.size(); i++)
    {
        auto x = *(itx + i);
        auto y = *(ity + i);
        _ir.valid_location_rc.push_back(y);
        _ir.valid_location_rc.push_back(x);
    }

    /*
    directionInDeg = atan2d(IyValid,IxValid);
    directionInDeg(directionInDeg<0) = directionInDeg(directionInDeg<0) + 360;
    [~,directionIndex] = min(abs(directionInDeg - [0:45:315]),[],2); % Quantize the direction to 4 directions (don't care about the sign)
    */

    _ir.direction_deg = get_direction_deg2(_ir.valid_gradient_x, _ir.valid_gradient_y); // used for debug only
    _ir.directions = get_direction2(_ir.valid_gradient_x, _ir.valid_gradient_y);

    /*dirsVec = [0,1; 1,1; 1,0; 1,-1]; % These are the 4 directions
    dirsVec = [dirsVec;-dirsVec];
    if 1
        % Take the right direction
        dirPerPixel = dirsVec(directionIndex,:);
        localRegion = locRC + dirPerPixel.*reshape(vec(-2:1),1,1,[]);
        localEdges = squeeze(interp2(iEdge,localRegion(:,2,:),localRegion(:,1,:)));
        isSupressed = localEdges(:,3) >= localEdges(:,2) & localEdges(:,3) >= localEdges(:,4);

        fraqStep = (-0.5*(localEdges(:,4)-localEdges(:,2))./(localEdges(:,4)+localEdges(:,2)-2*localEdges(:,3))); % The step we need to move to reach the subpixel gradient i nthe gradient direction
        fraqStep((localEdges(:,4)+localEdges(:,2)-2*localEdges(:,3))==0) = 0;

        locRCsub = locRC + fraqStep.*dirPerPixel;*/
    double directions[8][2] = { {0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1} };
    std::vector<double> direction_per_pixel_x; //used later when finding valid direction per pixel
    for (auto i = 0; i < _ir.directions.size(); i++)
    {
        int idx = _ir.directions[i];
        _ir.direction_per_pixel.push_back(directions[idx][0]);
        _ir.direction_per_pixel.push_back(directions[idx][1]);
        direction_per_pixel_x.push_back(directions[idx][0]);
    }
    double vec[4] = { -2,-1,0,1 }; // one pixel along gradient direction, 2 pixels against gradient direction

    auto loc_it = _ir.valid_location_rc.begin();
    auto dir_pp_it = _ir.direction_per_pixel.begin();

    for (auto k = 0; k < 4; k++)
    {
        for (auto i = 0; i < _ir.direction_per_pixel.size(); i++)
        {
            double val = *(loc_it + i) + *(dir_pp_it + i) * vec[k];
            _ir.local_region[k].push_back(val);
        }
    }
    for (auto k = 0; k < 4; k++)
    {
        for (auto i = 0; i < 2 * _ir.valid_location_rc_x.size(); i++)
        {
            _ir.local_region_y[k].push_back(*(_ir.local_region[k].begin() + i));
            i++;
            _ir.local_region_x[k].push_back(*(_ir.local_region[k].begin() + i));
        }
    }
    // interpolation 
    _ir.local_edges = interpolation(_ir.edges, _ir.local_region_x, _ir.local_region_y, 4, _ir.valid_location_rc_x.size(), _ir.width);

    // is suppressed
    _ir.is_supressed = is_suppressed(_ir.local_edges, _ir.valid_location_rc_x.size());


    /*fraqStep = (-0.5*(localEdges(:,4)-localEdges(:,2))./(localEdges(:,4)+localEdges(:,2)-2*localEdges(:,3))); % The step we need to move to reach the subpixel gradient i nthe gradient direction
       fraqStep((localEdges(:,4)+localEdges(:,2)-2*localEdges(:,3))==0) = 0;

       locRCsub = locRC + fraqStep.*dirPerPixel;

       % Calculate the Z gradient for thresholding
       localZx = squeeze(interp2(Zx,localRegion(:,2,2:3),localRegion(:,1,2:3)));
       localZy = squeeze(interp2(Zy,localRegion(:,2,2:3),localRegion(:,1,2:3)));
       zGrad = [mean(localZy,2) ,mean(localZx,2)];
       zGradInDirection = abs(sum(zGrad.*normr(dirPerPixel),2));
       % Take the z value of the closest part of the edge
       localZvalues = squeeze(interp2(frame.z,localRegion(:,2,:),localRegion(:,1,:)));

       zValuesForSubEdges = min(localZvalues,[],2);
       edgeSubPixel = fliplr(locRCsub);% From Row-Col to XY*/

    std::vector< double > ::iterator loc_edg_it = _ir.local_edges.begin();
    //std::vector<double > ::iterator loc_rc_sub_it = _depth.local_rc_subpixel.begin(); // locRCsub
    auto valid_loc_rc = _ir.valid_location_rc.begin(); // locRC
    auto dir_per_pixel_it = _ir.direction_per_pixel.begin(); // dirPerPixel

    std::vector< double > edge_sub_pixel_x;
    std::vector< double > edge_sub_pixel_y;

    for (auto i = 0; i < _ir.valid_location_rc_x.size(); i++)
    {
        double vec2 = *(loc_edg_it + 1);
        double vec3 = *(loc_edg_it + 2);
        double vec4 = *(loc_edg_it + 3);
        loc_edg_it += 4;

        // The step we need to move to reach the subpixel gradient in the gradient direction
        //%fraqStep = (-0.5*(localEdges(:,4) - localEdges(:,2)). / (localEdges(:,4) + localEdges(:,2) - 2 * localEdges(:,3)));
        //%fraqStep( (localEdges(:,4) + localEdges(:,2) - 2 * localEdges(:,3)) == 0 ) = 0;
        double const denom = vec4 + vec2 - 2 * vec3;
        double const res = ( denom == 0 ) ? 0 : ( -0.5 * ( vec4 - vec2 ) / denom );
        _ir.fraq_step.push_back( res );

        auto valx = *valid_loc_rc + *dir_per_pixel_it * res;
        valid_loc_rc++;
        dir_per_pixel_it++;
        auto valy = *valid_loc_rc + *dir_per_pixel_it * res;
        valid_loc_rc++;
        dir_per_pixel_it++;
        _z.local_rc_subpixel.push_back(valx);
        _z.local_rc_subpixel.push_back(valy);

        _z.edge_sub_pixel.push_back(valy);
        _z.edge_sub_pixel.push_back(valx);
        edge_sub_pixel_x.push_back(valy);
        edge_sub_pixel_y.push_back(valx);
    }

    std::vector<double> local_region_x[2] = { _ir.local_region_x[1] ,_ir.local_region_x[2] };
    std::vector<double> local_region_y[2] = { _ir.local_region_y[1] ,_ir.local_region_y[2] };
    _z.local_x = interpolation(_z.gradient_x, local_region_x, local_region_y, 2, _ir.valid_location_rc_x.size(), _z.width);
    _z.local_y = interpolation(_z.gradient_y, local_region_x, local_region_y, 2, _ir.valid_location_rc_x.size(), _z.width);
    _z.gradient = depth_mean(_z.local_x, _z.local_y);
    _z.grad_in_direction = sum_gradient_depth(_z.gradient, _ir.direction_per_pixel);
    _z.local_values = interpolation(_z.frame, _ir.local_region_x, _ir.local_region_y, 4, _ir.valid_location_rc_x.size(), _z.width);
    _z.values_for_subedges = find_local_values_min(_z.local_values);

    //_params.alpha;
    /* validEdgePixels = zGradInDirection > params.gradZTh & isSupressed & zValuesForSubEdges > 0;

   zGradInDirection = zGradInDirection(validEdgePixels);
   edgeSubPixel = edgeSubPixel(validEdgePixels,:);
   zValuesForSubEdges = zValuesForSubEdges(validEdgePixels);
   dirPerPixel = dirPerPixel(validEdgePixels);
   sectionMapDepth = sectionMapValid(validEdgePixels);
   directionIndex = directionIndex(validEdgePixels);
   directionIndex(directionIndex>4) = directionIndex(directionIndex>4)-4;% Like taking abosoulte value on the direction
   */
    _z.supressed_edges = find_valid_depth_edges( _z.grad_in_direction,
                                                 _ir.is_supressed,
                                                 _z.values_for_subedges,
                                                 _params.grad_z_threshold );
    std::vector<double> valid_values_for_subedges;



    depth_filter(_z.grad_in_direction_valid, _z.grad_in_direction, _z.supressed_edges, 1, _z.supressed_edges.size());
    depth_filter(_z.valid_edge_sub_pixel_x, edge_sub_pixel_x, _z.supressed_edges, 1, _z.supressed_edges.size()); //edgeSubPixel = edgeSubPixel(validEdgePixels,:);
    depth_filter(_z.valid_edge_sub_pixel_y, edge_sub_pixel_y, _z.supressed_edges, 1, _z.supressed_edges.size());
    for (auto i = 0; i < _z.valid_edge_sub_pixel_x.size(); i++)
    {
        _z.valid_edge_sub_pixel.push_back(*(_z.valid_edge_sub_pixel_x.begin() + i));
        _z.valid_edge_sub_pixel.push_back(*(_z.valid_edge_sub_pixel_y.begin() + i));
        // subPoints : subPoints = [xim,yim,ones(size(yim))];
        _z.sub_points.push_back(*(_z.valid_edge_sub_pixel_x.begin() + i)-1);
        _z.sub_points.push_back(*(_z.valid_edge_sub_pixel_y.begin() + i)-1);
        _z.sub_points.push_back(1);
    }
    depth_filter(valid_values_for_subedges, _z.values_for_subedges, _z.supressed_edges, 1, _z.supressed_edges.size());
    depth_filter(_z.valid_direction_per_pixel, direction_per_pixel_x, _z.supressed_edges, 1, _z.supressed_edges.size());
    depth_filter(_z.valid_section_map, _ir.valid_section_map, _z.supressed_edges, 1, _z.supressed_edges.size());
    std::vector<double> edited_ir_directions;

    for (auto i = 0; i < _ir.directions.size(); i++)
    {
        auto val = double(*(_ir.directions.begin() + i));
        val = val + 1;// +1 to align with matlab
        val = val > 4 ? val - 4 : val;
        edited_ir_directions.push_back(val);
    }
    depth_filter(_z.valid_directions, edited_ir_directions, _z.supressed_edges, 1, _z.supressed_edges.size());

    _z.values_for_subedges = valid_values_for_subedges;

    /* weights = min(max(zGradInDirection - params.gradZTh,0),params.gradZMax - params.gradZTh);
    if params.constantWeights
        weights(:) = params.constantWeightsValue;
    end
    xim = edgeSubPixel(:,1)-1;
    yim = edgeSubPixel(:,2)-1;

    subPoints = [xim,yim,ones(size(yim))];
    vertices = subPoints*(pinv(params.Kdepth)').*zValuesForSubEdges/single(params.zMaxSubMM);

    [uv,~,~] = OnlineCalibration.aux.projectVToRGB(vertices,params.rgbPmat,params.Krgb,params.rgbDistort);
    isInside = OnlineCalibration.aux.isInsideImage(uv,params.rgbRes);
   
    xim = xim(isInside);
    yim = yim(isInside);
    zValuesForSubEdges = zValuesForSubEdges(isInside);
    zGradInDirection = zGradInDirection(isInside);
    directionIndex = directionIndex(isInside);
    weights = weights(isInside);
    vertices = vertices(isInside,:);
    sectionMapDepth = sectionMapDepth(isInside);*/
    k_matrix k = depth_intrinsics;
    rotation k_depth_pinv = { 0 };
    pinv_3x3( k.as_3x3().rot, k_depth_pinv.rot );
    transform(_z.valid_edge_sub_pixel_x.begin(), _z.valid_edge_sub_pixel_x.end(), _z.valid_edge_sub_pixel_x.begin(), bind2nd(std::plus<double>(), -1.0));
    transform(_z.valid_edge_sub_pixel_y.begin(), _z.valid_edge_sub_pixel_y.end(), _z.valid_edge_sub_pixel_y.begin(), bind2nd(std::plus<double>(), -1.0));
    for (auto i = 0; i < _z.sub_points.size(); i += 3)
    {
        //%vertices = subPoints * pinv(params.Kdepth)' .* zValuesForSubEdges / params.zMaxSubMM;
        double sub_points_mult[3];
        double x = _z.sub_points[i];
        double y = _z.sub_points[i + 1];
        double z = _z.sub_points[i + 2];
        for (auto jj = 0; jj < 3; jj++)
        {
            sub_points_mult[jj] = x * k_depth_pinv.rot[3 * jj + 0]
                                + y * k_depth_pinv.rot[3 * jj + 1]
                                + z * k_depth_pinv.rot[3 * jj + 2];
        }
        auto z_value_for_subedge = _z.values_for_subedges[i / 3];
        auto val1 = sub_points_mult[0] * z_value_for_subedge / _params.max_sub_mm_z;
        auto val2 = sub_points_mult[1] * z_value_for_subedge / _params.max_sub_mm_z;
        auto val3 = sub_points_mult[2] * z_value_for_subedge / _params.max_sub_mm_z;
        _z.vertices_all.push_back( { val1, val2, val3 } );
    }
    _z.uvmap = get_texture_map( _z.vertices_all,
                                _original_calibration,
                                _original_calibration.calc_p_mat() );


    for (auto i = 0; i < _z.uvmap.size(); i++)
    {
        //%isInside = xy(:,1) >= 0 & ...
        //%           xy(:,1) <= res(2) - 1 & ...
        //%           xy(:,2) >= 0 & ...
        //%           xy(:,2) <= res(1) - 1;
        bool cond_x = (_z.uvmap[i].x >= 0) && (_z.uvmap[i].x <= _yuy.width-1);
        bool cond_y = (_z.uvmap[i].y >= 0) && (_z.uvmap[i].y <= _yuy.height-1);
        _z.is_inside.push_back( cond_x && cond_y );
    }

    /*xim = xim(isInside);
    yim = yim(isInside); 
    zValuesForSubEdges = zValuesForSubEdges(isInside);
    zGradInDirection = zGradInDirection(isInside);
    directionIndex = directionIndex(isInside);
    weights = weights(isInside);
    vertices = vertices(isInside,:);
    sectionMapDepth = sectionMapDepth(isInside);*/
    //std::vector<double> weights;
    for (auto i = 0; i < _z.is_inside.size(); i++) {

        _z.valid_weights.push_back(_params.constant_weights);
    }
    depth_filter(_z.subpixels_x, _z.valid_edge_sub_pixel_x, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.subpixels_y, _z.valid_edge_sub_pixel_y, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.closest, _z.values_for_subedges, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.grad_in_direction_inside, _z.grad_in_direction_valid, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.directions, _z.valid_directions, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.vertices, _z.vertices_all, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.section_map_depth_inside, _z.valid_section_map, _z.is_inside, 1, _z.is_inside.size());
    depth_filter(_z.weights, _z.valid_weights, _z.is_inside, 1, _z.is_inside.size());

    _z.relevant_pixels_image.resize(_z.width * _z.height, 0);
    std::vector<double> sub_pixel_x = _z.subpixels_x;
    std::vector<double> sub_pixel_y= _z.subpixels_y;

    transform(_z.subpixels_x.begin(), _z.subpixels_x.end(), sub_pixel_x.begin(), [](double x) {return round(x + 1); });
    transform(_z.subpixels_y.begin(), _z.subpixels_y.end(), sub_pixel_y.begin(), [](double x) {return round(x + 1); });

    _z.subpixels_y_round = sub_pixel_y;
    _z.subpixels_x_round = sub_pixel_x;

    for (auto i = 0; i < sub_pixel_x.size(); i++)
    {
        auto x = _z.subpixels_x_round[i];
        auto y = _z.subpixels_y_round[i];

        _z.relevant_pixels_image[size_t( ( y - 1 ) * _z.width + x - 1 )] = 1;
    }
}


void optimizer::set_yuy_data(
    std::vector< yuy_t > && yuy_data,
    std::vector< yuy_t > && prev_yuy_data,
    calib const & calibration
)
{
    _original_calibration = calibration;

    _yuy.width = calibration.width;
    _yuy.height = calibration.height;
    _params.set_rgb_resolution( _yuy.width, _yuy.height );

    _yuy.orig_frame = std::move( yuy_data );
    _yuy.prev_frame = std::move( prev_yuy_data );

    _yuy.lum_frame = get_luminance_from_yuy2( _yuy.orig_frame );
    _yuy.prev_lum_frame = get_luminance_from_yuy2( _yuy.prev_frame );

    _yuy.edges = calc_edges( _yuy.lum_frame, _yuy.width, _yuy.height );
    _yuy.prev_edges = calc_edges(_yuy.prev_lum_frame, _yuy.width, _yuy.height);

    _yuy.edges_IDT = blur_edges( _yuy.edges, _yuy.width, _yuy.height );

    _yuy.edges_IDTx = calc_vertical_gradient( _yuy.edges_IDT, _yuy.width, _yuy.height );

    _yuy.edges_IDTy = calc_horizontal_gradient( _yuy.edges_IDT, _yuy.width, _yuy.height );
}

void optimizer::set_ir_data(
    std::vector< ir_t > && ir_data,
    size_t width,
    size_t height
)
{
    _ir.width = width;
    _ir.height = height;
    
    _ir.ir_frame = std::move( ir_data );
    _ir.edges = calc_edges( _ir.ir_frame, width, height );
}

calib optimizer::decompose_p_mat(p_matrix p)
{
    auto calib = decompose(p, _original_calibration);
    return calib;
}


rs2_intrinsics_double optimizer::get_new_z_intrinsics_from_new_calib(const rs2_intrinsics_double& orig, const calib & new_c, const calib & orig_c)
{
    rs2_intrinsics_double res;
    res = orig;
    res.fx = res.fx / new_c.k_mat.fx*orig_c.k_mat.fx;
    res.fy = res.fy / new_c.k_mat.fy*orig_c.k_mat.fy;

    return res;
}

void optimizer::zero_invalid_edges( z_frame_data & z_data, ir_frame_data const & ir_data )
{
    for( auto i = 0; i < ir_data.edges.size(); i++ )
    {
        if( ir_data.edges[i] <= _params.grad_ir_threshold || z_data.edges[i] <= _params.grad_z_threshold )
        {
            z_data.supressed_edges[i] = 0;
            z_data.subpixels_x[i] = 0;
            z_data.subpixels_y[i] = 0;
            z_data.closest[i] = 0;
        }
    }
}

std::vector< direction > optimizer::get_direction( std::vector<double> gradient_x, std::vector<double> gradient_y )
{
    std::vector<direction> res( gradient_x.size(), deg_none );

    std::map<int, direction> angle_dir_map = { {0, deg_0}, {45,deg_45} , {90,deg_90}, {135,deg_135} };

    for( auto i = 0; i < gradient_x.size(); i++ )
    {
        int closest = -1;
        auto angle = atan2( gradient_y[i], gradient_x[i] )* 180.f / M_PI;
        angle = angle < 0 ? 180 + angle : angle;
        auto dir = fmod( angle, 180 );

        for( auto d : angle_dir_map )
        {
            closest = closest == -1 || abs( dir - d.first ) < abs( dir - closest ) ? d.first : closest;
        }
        res[i] = angle_dir_map[closest];
    }
    return res;
}
std::vector< direction > optimizer::get_direction2(std::vector<double> gradient_x, std::vector<double> gradient_y)
{
    std::vector<direction> res(gradient_x.size(), deg_none);
    
    std::map<int, direction> angle_dir_map = { {0, deg_0}, {45,deg_45} , {90,deg_90}, {135,deg_135} , { 180,deg_180 }, { 225,deg_225 }, { 270,deg_270 }, { 315,deg_315 } };



    for (auto i = 0; i < gradient_x.size(); i++)
    {
        int closest = -1;
        auto angle = atan2(gradient_y[i], gradient_x[i]) * 180.f / M_PI;
        angle = angle < 0 ? 360 + angle : angle;
        auto dir = fmod(angle, 360);

        for (auto d : angle_dir_map)
        {
            closest = closest == -1 || abs(dir - d.first) < abs(dir - closest) ? d.first : closest;
        }
        res[i] = angle_dir_map[closest];
    }
    return res;
}
//std::vector< uint16_t > optimizer::get_closest_edges(
//    z_frame_data const & z_data,
//    ir_frame_data const & ir_data,
//    size_t width, size_t height )
//{
//    std::vector< uint16_t > z_closest;
//    z_closest.reserve( z_data.edges.size() );
//
//    for( auto i = 0; i < int(height); i++ )
//    {
//        for( auto j = 0; j < int(width); j++ )
//        {
//            auto idx = i * width + j;
//
//            auto edge = z_data.edges[idx];
//
//            //if (edge == 0)  continue;
//
//            auto edge_prev_idx = get_prev_index( z_data.valid_directions[idx], i, j, width, height );
//
//            auto edge_next_idx = get_next_index( z_data.valid_directions[idx], i, j, width, height );
//
//            auto edge_minus_idx = edge_prev_idx.second * width + edge_prev_idx.first;
//
//            auto edge_plus_idx = edge_next_idx.second * width + edge_next_idx.first;
//
//            auto z_edge_plus = z_data.edges[edge_plus_idx];
//            auto z_edge = z_data.edges[idx];
//            auto z_edge_minus = z_data.edges[edge_minus_idx];
//
//           
//            if (z_data.supressed_edges[idx])
//            {
//                z_closest.push_back(std::min(z_data.frame[edge_minus_idx], z_data.frame[edge_plus_idx]));
//            }
//            else
//            {
//                z_closest.push_back(0);
//            }
//        }
//    }
//    return z_closest;
//}

/* Given pixel coordinates and depth in an image with no distortion or inverse distortion coefficients, compute the corresponding point in 3D space relative to the same camera */
static void deproject_pixel_to_point(double point[3], const struct rs2_intrinsics_double * intrin, const double pixel[2], double depth)
{
    double x = (double)(pixel[0] - intrin->ppx) / intrin->fx;
    double y = (double)(pixel[1] - intrin->ppy) / intrin->fy;

    point[0] = depth * x;
    point[1] = depth * y;
    point[2] = depth;
}

/* Given a point in 3D space, compute the corresponding pixel coordinates in an image with no distortion or forward distortion coefficients produced by the same camera */
static void project_point_to_pixel(double pixel[2], const struct rs2_intrinsics_double * intrin, const double point[3])
{
    double x = point[0] / point[2], y = point[1] / point[2];

    if( intrin->model == RS2_DISTORTION_BROWN_CONRADY )
    {
        double r2 = x * x + y * y;
        double f = 1 + intrin->coeffs[0] * r2 + intrin->coeffs[1] * r2*r2 + intrin->coeffs[4] * r2*r2*r2;

        double xcd = x * f;
        double ycd = y * f;

        double dx = xcd + 2 * intrin->coeffs[2] * x*y + intrin->coeffs[3] * (r2 + 2 * x*x);
        double dy = ycd + 2 * intrin->coeffs[3] * x*y + intrin->coeffs[2] * (r2 + 2 * y*y);

        x = dx;
        y = dy;
    }

    pixel[0] = x * (double)intrin->fx + (double)intrin->ppx;
    pixel[1] = y * (double)intrin->fy + (double)intrin->ppy;
}

std::vector<double> optimizer::blur_edges(std::vector<double> const & edges, size_t image_width, size_t image_height)
{
    std::vector<double> res = edges;

    for( auto i = 0; i < image_height; i++ )
        for( auto j = 0; j < image_width; j++ )
        {
            if( i == 0 && j == 0 )
                continue;
            else if( i == 0 )
                res[j] = std::max( res[j], res[j - 1] * _params.gamma );
            else if( j == 0 )
                res[i*image_width + j] = std::max(
                    res[i*image_width + j],
                    res[(i - 1)*image_width + j] * _params.gamma );
            else
                res[i*image_width + j] = std::max(
                    res[i*image_width + j],
                    std::max(
                        res[ i     *image_width + j - 1] * _params.gamma,
                        res[(i - 1)*image_width + j    ] * _params.gamma ) );
        }


    for( int i = int(image_height) - 1; i >= 0; i-- )  // note: must be signed because will go under 0!
        for( int j = int(image_width) - 1; j >= 0; j-- )
        {
            if( i == image_height - 1 && j == image_width - 1 )
                continue;
            else if( i == image_height - 1 )
                res[i*image_width + j] = std::max( res[i*image_width + j], res[i*image_width + j + 1] * _params.gamma );
            else if( j == image_width - 1 )
                res[i*image_width + j] = std::max( res[i*image_width + j], res[(i + 1)*image_width + j] * _params.gamma );
            else
                res[i*image_width + j] = std::max( res[i*image_width + j], (std::max( res[i*image_width + j + 1] * _params.gamma, res[(i + 1)*image_width + j] * _params.gamma )) );
        }

    for( auto i = 0; i < image_height; i++ )
        for( auto j = 0; j < image_width; j++ )
            res[i*image_width + j] = _params.alpha * edges[i*image_width + j] + (1 - _params.alpha) * res[i*image_width + j];
    return res;
}


std::vector< byte > optimizer::get_luminance_from_yuy2( std::vector< yuy_t > const & yuy2_imagh )
{
    std::vector<byte> res( yuy2_imagh.size(), 0 );
    auto yuy2 = (uint8_t*)yuy2_imagh.data();
    for( auto i = 0; i < res.size(); i++ )
        res[i] = yuy2[i * 2];

    return res;
}

std::vector<uint8_t> optimizer::get_logic_edges( std::vector<double> edges )
{
    std::vector<uint8_t> logic_edges( edges.size(), 0 );
    auto max = std::max_element( edges.begin(), edges.end() );
    auto thresh = *max*_params.edge_thresh4_logic_lum;

    for( auto i = 0; i < edges.size(); i++ )
    {
        logic_edges[i] = abs( edges[i] ) > thresh ? 1 : 0;
    }
    return logic_edges;
}



void optimizer::sum_per_section(
    std::vector< double > & sum_weights_per_section,
    std::vector< byte > const & section_map,
    std::vector< double > const & weights,
    size_t num_of_sections
)
{/*sumWeightsPerSection = zeros(params.numSectionsV*params.numSectionsH,1);
for ix = 1:params.numSectionsV*params.numSectionsH
    sumWeightsPerSection(ix) = sum(weights(sectionMap == ix-1));
end*/
    sum_weights_per_section.resize( num_of_sections );
    auto p_sum = sum_weights_per_section.data();
    for( byte i = 0; i < num_of_sections; ++i, ++p_sum )
    {
        *p_sum = 0;

        auto p_section = section_map.data();
        auto p_weight = weights.data();
        for( size_t ii = 0; ii < section_map.size(); ++ii, ++p_section, ++p_weight )
        {
            if( *p_section == i )
                *p_sum += *p_weight;
        }
    }
}


double get_max(double x, double y)
{
    return x > y ? x : y;
}
double get_min(double x, double y)
{
    return x < y ? x : y;
}

//std::vector<double> optimizer::calculate_weights(z_frame_data& z_data)
//{
//    std::vector<double> res;
//
//    for (auto i = 0; i < z_data.supressed_edges.size(); i++)
//    {
//        if (z_data.supressed_edges[i])
//            z_data.weights.push_back(
//                get_min(get_max(z_data.supressed_edges[i] - _params.grad_z_min, (double)0),
//                    _params.grad_z_max - _params.grad_z_min));
//    }
//
//    return res;
//}

void deproject_sub_pixel(
    std::vector<double3>& points,
    const rs2_intrinsics_double& intrin,
    std::vector< byte > const & valid_edges,
    const double* x,
    const double* y,
    const double* depth, double depth_units
)
{
    auto ptr = (double*)points.data();
    byte const * valid_edge = valid_edges.data();
    for (size_t i = 0; i < valid_edges.size(); ++i)
    {
        if (!valid_edge[i])
            continue;

        const double pixel[] = { x[i] - 1, y[i] - 1 };
        deproject_pixel_to_point(ptr, &intrin, pixel, depth[i] * depth_units);
        ptr += 3;
    }
}

std::vector<double3> optimizer::subedges2vertices(z_frame_data& z_data, const rs2_intrinsics_double& intrin, double depth_units)
{
    std::vector<double3> res(z_data.n_strong_edges);
    deproject_sub_pixel(res, intrin, z_data.supressed_edges, z_data.subpixels_x.data(), z_data.subpixels_y.data(), z_data.closest.data(), depth_units);
    z_data.vertices = res;
    return res;
}

static p_matrix calc_p_gradients(const z_frame_data & z_data, 
    const yuy2_frame_data & yuy_data, 
    std::vector<double> interp_IDT_x, 
    std::vector<double> interp_IDT_y,
    const calib & cal,
    const p_matrix & p_mat,
    const std::vector<double>& rc, 
    const std::vector<double2>& xy,
    iteration_data_collect * data = nullptr)
{
    auto coefs = calc_p_coefs(z_data, yuy_data, cal, p_mat, rc, xy);
    auto w = z_data.weights;

    if (data)
        data->coeffs_p = coefs;

    p_matrix sums = { 0 };
    auto sum_of_valids = 0;

    for (auto i = 0; i < coefs.x_coeffs.size(); i++)
    {
        if (interp_IDT_x[i] == std::numeric_limits<double>::max() || interp_IDT_y[i] == std::numeric_limits<double>::max())
            continue;

        sum_of_valids++;

        for (auto j = 0; j < 12; j++)
        {
            sums.vals[j] += w[i] * (interp_IDT_x[i] * coefs.x_coeffs[i].vals[j] + interp_IDT_y[i] * coefs.y_coeffs[i].vals[j]);
        }
        
    }

    p_matrix averages = { 0 };
    for (auto i = 0; i < 8; i++) //zero the last line of P grad?
    {
        averages.vals[i] = (double)sums.vals[i] / (double)sum_of_valids;
    }

    return averages;
}

static
std::pair< std::vector<double2>, std::vector<double>> calc_rc(
    const z_frame_data & z_data,
    const yuy2_frame_data & yuy_data,
    const calib & cal,
    const p_matrix & p_mat
)
{
    auto v = z_data.vertices;

    std::vector<double2> f1( z_data.vertices.size() );
    std::vector<double> r2( z_data.vertices.size() );
    std::vector<double> rc( z_data.vertices.size() );

    auto yuy_intrin = cal.get_intrinsics();
    auto yuy_extrin = cal.get_extrinsics();

    auto fx = (double)yuy_intrin.fx;
    auto fy = (double)yuy_intrin.fy;
    auto ppx = (double)yuy_intrin.ppx;
    auto ppy = (double)yuy_intrin.ppy;

    auto & r = yuy_extrin.rotation;
    auto & t = yuy_extrin.translation;

   /* double mat[3][4] = {
        fx*(double)r[0] + ppx * (double)r[2], fx*(double)r[3] + ppx * (double)r[5], fx*(double)r[6] + ppx * (double)r[8], fx*(double)t[0] + ppx * (double)t[2],
        fy*(double)r[1] + ppy * (double)r[2], fy*(double)r[4] + ppy * (double)r[5], fy*(double)r[7] + ppy * (double)r[8], fy*(double)t[1] + ppy * (double)t[2],
        r[2], r[5], r[8], t[2] };
*/
    auto mat = p_mat.vals;
    for( auto i = 0; i < z_data.vertices.size(); ++i )
    {
        double x = v[i].x;
        double y = v[i].y;
        double z = v[i].z;

        double x1 = (double)mat[0] * (double)x + (double)mat[1] * (double)y + (double)mat[2] * (double)z + (double)mat[3];
        double y1 = (double)mat[4] * (double)x + (double)mat[5] * (double)y + (double)mat[6] * (double)z + (double)mat[7];
        double z1 = (double)mat[8] * (double)x + (double)mat[9] * (double)y + (double)mat[10] * (double)z + (double)mat[11];

        auto x_in = x1 / z1;
        auto y_in = y1 / z1;

        auto x2 = ((x_in - ppx) / fx);
        auto y2 = ((y_in - ppy) / fy);

        f1[i].x = x2;
        f1[i].y = y2;

        auto r2 = (x2 * x2 + y2 * y2);

        rc[i] = 1 + (double)yuy_intrin.coeffs[0] * r2 + (double)yuy_intrin.coeffs[1] * r2 * r2 + (double)yuy_intrin.coeffs[4] * r2 * r2 * r2;
    }

    return { f1,rc };
}

static p_matrix calc_gradients(
    const z_frame_data& z_data,
    const yuy2_frame_data& yuy_data,
    const std::vector<double2>& uv,
    const calib & cal,
    const p_matrix & p_mat,
    iteration_data_collect * data = nullptr
)
{
    p_matrix res;
    auto interp_IDT_x = biliniar_interp( yuy_data.edges_IDTx, yuy_data.width, yuy_data.height, uv );      
    auto interp_IDT_y = biliniar_interp( yuy_data.edges_IDTy, yuy_data.width, yuy_data.height, uv );

    auto rc = calc_rc( z_data, yuy_data, cal, p_mat);

    if (data)
    {
        data->d_vals_x = interp_IDT_x;
        data->d_vals_y = interp_IDT_y;
        data->xy = rc.first;
        data->rc = rc.second;
    }
        
    res = calc_p_gradients( z_data, yuy_data, interp_IDT_x, interp_IDT_y, cal, p_mat, rc.second, rc.first, data );
    return res;
}

std::pair<double, p_matrix> calc_cost_and_grad(
    const z_frame_data & z_data,
    const yuy2_frame_data & yuy_data,
    const calib & cal,
    const p_matrix & p_mat,
    iteration_data_collect * data = nullptr
)
{
    auto uvmap = get_texture_map(z_data.vertices, cal, p_mat);
    if( data )
        data->uvmap = uvmap;

    auto cost = calc_cost(z_data, yuy_data, uvmap, data ? &data->d_vals : nullptr );
    auto grad = calc_gradients(z_data, yuy_data, uvmap, cal, p_mat, data);
    return { cost, grad };
}

params::params()
{
    normalize_mat.vals[0] = 0.353692440000000;
    normalize_mat.vals[1] = 0.266197740000000;
    normalize_mat.vals[2] = 1.00926010000000;
    normalize_mat.vals[3] = 0.000673204490000000;
    normalize_mat.vals[4] = 0.355085250000000;
    normalize_mat.vals[5] = 0.266275050000000;
    normalize_mat.vals[6] = 1.01145800000000;
    normalize_mat.vals[7] = 0.000675013750000000;
    normalize_mat.vals[8] = 414.205570000000;
    normalize_mat.vals[9] = 313.341060000000;
    normalize_mat.vals[10] = 1187.34590000000;
    normalize_mat.vals[11] = 0.791570250000000;

    // NOTE: until we know the resolution, the current state is just the default!
    // We need to get the depth and rgb resolutions to make final decisions!
}
svm_model_linear::svm_model_linear()
{
}
svm_model_gaussian::svm_model_gaussian()
{
}
void params::set_depth_resolution( size_t width, size_t height )
{
    AC_LOG( DEBUG, "... depth resolution= " << width << "x" << height );
    // Some parameters are resolution-dependent
    bool const XGA = (width == 1024 && height == 768);
    if( XGA )
    {
        AC_LOG( DEBUG, "... changing IR threshold: " << grad_ir_threshold << " -> " << 2.5 << "  (because of resolution)" );
        grad_ir_threshold = 2.5;
    }
}

void params::set_rgb_resolution( size_t width, size_t height )
{
    AC_LOG( DEBUG, "... RGB resolution= " << width << "x" << height );
}

calib const & optimizer::get_calibration() const
{
    return _final_calibration;
}

rs2_dsm_params const & optimizer::get_dsm_params() const
{
    return _final_dsm_params;
}

double optimizer::get_cost() const
{
    return _params_curr.cost;
}

static
void write_to_file( void const * data, size_t cb,
    std::string const & dir,
    char const * filename
)
{
    std::string path = dir + '\\' + filename;
    std::fstream f( path, std::ios::out | std::ios::binary );
    if( !f )
        throw std::runtime_error( "failed to open file:\n" + path );
    f.write( (char const *) data, cb );
    f.close();
}

template< typename T >
void write_obj( std::fstream & f, T const & o )
{
    f.write( (char const *)&o, sizeof( o ) );
}

template< typename T >
void write_vector_to_file( std::vector< T > const & v,
    std::string const & dir,
    char const * filename
)
{
    write_to_file( v.data(), v.size() * sizeof( T ), dir, filename );
}

void write_matlab_camera_params_file(
    rs2_intrinsics const & _intr_depth,
    calib const & rgb_calibration,
    float _depth_units,
    std::string const & dir,
    char const * filename
)
{
    std::string path = dir + '\\' + filename;
    std::fstream f( path, std::ios::out | std::ios::binary );
    if( !f )
        throw std::runtime_error( "failed to open file:\n" + path );


    //depth intrinsics
    write_obj( f, (double)_intr_depth.width );
    write_obj( f, (double)_intr_depth.height );
    write_obj( f, (double)_depth_units );

    double k_depth[9] = { _intr_depth.fx, 0, _intr_depth.ppx,
                        0, _intr_depth.fy, _intr_depth.ppy,
                        0, 0, 1 };
    for( auto i = 0; i < 9; i++ )
    {
        write_obj( f, k_depth[i] );
    }

    //color intrinsics
    rs2_intrinsics _intr_rgb = rgb_calibration.get_intrinsics();
    
    write_obj( f, (double)_intr_rgb.width );
    write_obj( f, (double)_intr_rgb.height );

    double k_rgb[9] = { _intr_rgb.fx, 0, _intr_rgb.ppx,
                        0, _intr_rgb.fy, _intr_rgb.ppy,
                        0, 0, 1 };


    for( auto i = 0; i < 9; i++ )
    {
        write_obj( f, k_rgb[i] );
    }

    for( auto i = 0; i < 5; i++ )
    {
        write_obj( f, (double)_intr_rgb.coeffs[i] );
    }

    //extrinsics
    rs2_extrinsics _extr = rgb_calibration.get_extrinsics();
    for( auto i = 0; i < 9; i++ )
    {
        write_obj( f, (double)_extr.rotation[i] );
    }
    //extrinsics
    for( auto i = 0; i < 3; i++ )
    {
        write_obj( f, (double)_extr.translation[i] );
    }

    f.close();
}

void optimizer::write_data_to( std::string const & dir )
{
    AC_LOG( DEBUG, "... writing data to: " << dir );
    
    try
    {
        write_vector_to_file( _yuy.orig_frame, dir, "rgb.raw" );
        write_vector_to_file( _yuy.prev_frame, dir, "rgb_prev.raw" );
        write_vector_to_file( _ir.ir_frame, dir, "ir.raw" );
        write_vector_to_file( _z.frame, dir, "depth.raw" );

        write_to_file( &_original_dsm_params, sizeof( _original_dsm_params ), dir, "dsm.params" );
        write_to_file( &_original_calibration, sizeof( _original_calibration ), dir, "rgb.calib" );
        auto & cal_info = _k_to_DSM->get_calibration_info();
        auto & cal_regs = _k_to_DSM->get_calibration_registers();
        write_to_file( &cal_info, sizeof( cal_info ), dir, "cal.info" );
        write_to_file( &cal_regs, sizeof( cal_regs ), dir, "cal.registers" );
        write_to_file( &_z.orig_intrinsics, sizeof( _z.orig_intrinsics), dir, "depth.intrinsics" );
        write_to_file( &_z.depth_units, sizeof( _z.depth_units ), dir, "depth.units" );

        // This file is meant for matlab -- it packages all the information needed
        write_matlab_camera_params_file( _z.orig_intrinsics,
                                         _original_calibration,
                                         _z.depth_units,
                                         dir,
                                         "camera_params"
        );
    }
    catch( std::exception const & err )
    {
        AC_LOG( ERROR, "Failed to write data: " << err.what() );
    }
    catch( ... )
    {
        AC_LOG( ERROR, "Failed to write data (unknown error)" );
    }
}

optimization_params optimizer::back_tracking_line_search( optimization_params const & curr_params,
                                                          iteration_data_collect * data ) const
{
    optimization_params new_params;

    // was gradStruct.P ./ norm( gradStruct.P(:) )   -> vector norm
    // now gradStruct.P ./ norm( gradStruct.P )      -> matrix 2-norm
    auto grads_over_norm
        = curr_params.calib_gradients.normalize( curr_params.calib_gradients.matrix_norm() );
    //%grad = gradStruct.P ./ norm(gradStruct.P) ./ params.rgbPmatNormalizationMat;
    auto grad = grads_over_norm / _params.normalize_mat;

    //%unitGrad = grad ./ norm(grad);     <-   was ./ norm(grad(:)')
    auto grad_norm = grad.matrix_norm();
    auto unit_grad = grad.normalize( grad_norm );

    //%t = -params.controlParam * grad(:)' * unitGrad(:);
    // -> dot product of grad and unitGrad
    auto t_vals = ( grad * -_params.control_param ) * unit_grad;
    auto t = t_vals.sum();

    //%stepSize = params.maxStepSize * norm(grad) / norm(unitGrad);
    auto step_size = _params.max_step_size * grad_norm / unit_grad.matrix_norm();


    auto movement = unit_grad * step_size;
    new_params.curr_p_mat = curr_params.curr_p_mat + movement;
    
    calib old_calib = decompose( curr_params.curr_p_mat, _original_calibration );
    auto uvmap_old = get_texture_map( _z.vertices, old_calib, curr_params.curr_p_mat );
    //curr_params.cost = calc_cost( z_data, yuy_data, uvmap_old );

    calib new_calib = decompose( new_params.curr_p_mat, _original_calibration );
    auto uvmap_new = get_texture_map( _z.vertices, new_calib, new_params.curr_p_mat );
    new_params.cost = calc_cost( _z, _yuy, uvmap_new );

    auto diff = calc_cost_per_vertex_diff( _z, _yuy, uvmap_old, uvmap_new );

    auto iter_count = 0;
    while( diff >= step_size * t
           && abs( step_size ) > _params.min_step_size
           && iter_count++ < _params.max_back_track_iters )
    {
        AC_LOG( DEBUG, "    back tracking line search cost= " << AC_D_PREC << new_params.cost );
        step_size = _params.tau * step_size;

        new_params.curr_p_mat = curr_params.curr_p_mat + unit_grad * step_size;
        
        new_calib = decompose( new_params.curr_p_mat, _original_calibration );
        uvmap_new = get_texture_map( _z.vertices, new_calib, new_params.curr_p_mat);
        new_params.cost = calc_cost( _z, _yuy, uvmap_new);
        diff = calc_cost_per_vertex_diff( _z, _yuy, uvmap_old, uvmap_new );
    }

    if(diff >= step_size * t )
    {
        new_params = curr_params;
    }

    if (data)
    {
        data->grads_norma = curr_params.calib_gradients.get_norma();
        data->grads_norm = grads_over_norm;
        data->normalized_grads = grad;
        data->unit_grad = unit_grad;
        data->back_tracking_line_search_iters = iter_count;
        data->t = t;
    }
    return new_params;
}

size_t optimizer::optimize_p
(
    const optimization_params& params_curr,
    optimization_params& params_new, 
    calib& new_rgb_calib,
    rs2_intrinsics_double& new_z_k,
    std::function<void(iteration_data_collect const&data)> cb,
    iteration_data_collect* data 
)
{
    size_t n_iterations = 0;
    auto curr = params_curr;
    while (1)
    {

        auto res = calc_cost_and_grad(_z, _yuy, new_rgb_calib, curr.curr_p_mat, data);
        curr.cost = res.first;
        curr.calib_gradients = res.second;
        AC_LOG( DEBUG, "    ------>     " << n_iterations << ": cost= " << AC_D_PREC << curr.cost );

        if (data)
        {
            data->type = iteration_data;
            data->params = curr;
            data->c = new_rgb_calib;
            data->iteration = n_iterations;
        }

        params_new = back_tracking_line_search(curr, data);
        
        if (data)
            data->next_params = params_new;

        if (cb)
            cb(*data);

        auto norm = (params_new.curr_p_mat - curr.curr_p_mat).get_norma();
        if (norm < _params.min_rgb_mat_delta)
        {
            AC_LOG(DEBUG, "... {normal(new-curr)} " << norm << " < " << _params.min_rgb_mat_delta << " {min_rgb_mat_delta}  -->  stopping");
            break;
        }

        auto delta = params_new.cost - curr.cost;
        AC_LOG( DEBUG, "    delta= " << AC_D_PREC << delta );
        delta = abs(delta);
        if (delta < _params.min_cost_delta)
        {
            AC_LOG(DEBUG, "... delta < " << _params.min_cost_delta << "  -->  stopping");
            break;
        }

        if (++n_iterations >= _params.max_optimization_iters)
        {
            AC_LOG(DEBUG, "... exceeding max iterations  -->  stopping");
            break;
        }

        curr = params_new;
        new_rgb_calib = decompose_p_mat(params_new.curr_p_mat);
    }

    if (!n_iterations)
    {
        AC_LOG(INFO, "Calibration not necessary; nothing done");
    }
    else
    {
        AC_LOG(INFO, "Calibration finished after " << n_iterations << " iterations; original cost= " << params_curr.cost << "  optimized cost= " << params_new.cost);
    }
    new_rgb_calib = decompose_p_mat(params_new.curr_p_mat);
    auto orig_rgb_calib = decompose_p_mat(params_curr.curr_p_mat);
    new_z_k = get_new_z_intrinsics_from_new_calib(_z.orig_intrinsics, new_rgb_calib, orig_rgb_calib);
    new_rgb_calib.k_mat.fx = _original_calibration.k_mat.fx;
    new_rgb_calib.k_mat.fy = _original_calibration.k_mat.fy;

    params_new.curr_p_mat = new_rgb_calib.calc_p_mat();
    return n_iterations;
}

size_t optimizer::optimize( std::function< void( iteration_data_collect const & data ) > cb )
{
    optimization_params params_orig;
    params_orig.curr_p_mat = _original_calibration.calc_p_mat();
    _original_calibration = decompose(params_orig.curr_p_mat, _original_calibration);
    _params_curr = params_orig;

    iteration_data_collect data;

    auto cycle = 1;
    data.cycle = cycle;

    auto res = calc_cost_and_grad(_z, _yuy, decompose(_params_curr.curr_p_mat, _original_calibration), _params_curr.curr_p_mat, &data);
    _params_curr.cost = res.first;
    _params_curr.calib_gradients = res.second;

    optimization_params new_params;
    calib new_calib = _original_calibration;
    rs2_intrinsics_double new_k_depth;
    double last_cost = _params_curr.cost;

    auto n_iterations = optimize_p(_params_curr, new_params, new_calib, new_k_depth, cb, &data);
    AC_LOG(DEBUG, n_iterations << ": Cost = " << AC_D_PREC << new_params.cost);

    _z.orig_vertices = _z.vertices;
    rs2_dsm_params_double new_dsm_params = _z.orig_dsm_params;
    while (cycle < _params.max_K2DSM_iters)
    {
        data.cycle = ++cycle;

        std::vector<double3> new_vertices;
        auto dsm_candidate = _k_to_DSM->convert_new_k_to_DSM(_z.orig_intrinsics, new_k_depth, _z, new_vertices, &data);
        data.type = cycle_data;

        data.cycle_data_p.dsm_params_cand = dsm_candidate;
        data.cycle_data_p.vertices = new_vertices;
        data.cycle_data_p.dsm_pre_process_data = _k_to_DSM->get_pre_process_data();

        if (cb)
            cb(data);

        _z.vertices = new_vertices;

        optimization_params params_candidate;
        calib calib_candidate = new_calib;
        rs2_intrinsics_double k_depth_candidate;
        optimize_p(new_params, params_candidate, calib_candidate, k_depth_candidate, cb, &data);

        if (params_candidate.cost < last_cost)
            break;

        new_params = params_candidate;
        new_calib = calib_candidate;
        new_k_depth = k_depth_candidate;
        new_dsm_params = dsm_candidate;
        last_cost = new_params.cost;
    }
   
    AC_LOG(INFO, "Calibration converged; cost= " << new_params.cost);

    _final_dsm_params = clip_ac_scaling(_z.orig_dsm_params, new_dsm_params);
    _final_calibration = new_calib;

    return n_iterations;
}
