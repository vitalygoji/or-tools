// Copyright 2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
import static java.lang.Math.abs;

import com.google.ortools.constraintsolver.Assignment;
import com.google.ortools.constraintsolver.FirstSolutionStrategy;
import com.google.ortools.constraintsolver.IntIntToLong;
import com.google.ortools.constraintsolver.RoutingDimension;
import com.google.ortools.constraintsolver.RoutingIndexManager;
import com.google.ortools.constraintsolver.RoutingModel;
import com.google.ortools.constraintsolver.RoutingDimension;
import com.google.ortools.constraintsolver.RoutingSearchParameters;
import com.google.ortools.constraintsolver.main;
import java.io.*;

class DataProblem {
  private int[][] locations_;

  public DataProblem() {
    locations_ =
        new int[][] {
          {4, 4}, {2, 0}, {8, 0}, {0, 1}, {1, 1}, {5, 2}, {7, 2}, {3, 3}, {6, 3}, {5, 5}, {8, 5},
          {1, 6}, {2, 6}, {3, 7}, {6, 7}, {0, 8}, {7, 8}
        };

    // Compute locations in meters using the block dimension defined as follow
    // Manhattan average block: 750ft x 264ft -> 228m x 80m
    // here we use: 114m x 80m city block
    // src: https://nyti.ms/2GDoRIe "NY Times: Know Your distance"
    int[] cityBlock = {228 / 2, 80};
    for (int i = 0; i < locations_.length; i++) {
      locations_[i][0] = locations_[i][0] * cityBlock[0];
      locations_[i][1] = locations_[i][1] * cityBlock[1];
    }
  }

  /// @brief Gets the number of vehicles.
  public int getVehicleNumber() {
    return 4;
  }
  /// @brief Gets the locations.
  public int[][] getLocations() {
    return locations_;
  }
  /// @brief Gets the number of locations.
  public int getLocationNumber() {
    return locations_.length;
  }
  /// @brief Gets the depot NodeIndex.
  public int getDepot() {
    return 0;
  }
}

/// @brief Manhattan distance implemented as a callback.
/// @details It uses an array of positions and computes
/// the Manhattan distance between the two positions of
/// two different indices.
class ManhattanDistance extends IntIntToLong {
  private int[][] distances;
  private RoutingIndexManager indexManager;

  public ManhattanDistance(DataProblem data, RoutingIndexManager manager) {
    // precompute distance between location to have distance callback in O(1)
    distances = new int[data.getLocationNumber()][data.getLocationNumber()];
    indexManager = manager;
    for (int fromNode = 0; fromNode < data.getLocationNumber(); ++fromNode) {
      for (int toNode = 0; toNode < data.getLocationNumber(); ++toNode) {
        if (fromNode == toNode) distances[fromNode][toNode] = 0;
        else
          distances[fromNode][toNode] =
              abs(data.getLocations()[toNode][0] - data.getLocations()[fromNode][0])
                  + abs(data.getLocations()[toNode][1] - data.getLocations()[fromNode][1]);
      }
    }
  }

  @Override
  /// @brief Returns the manhattan distance between the two nodes.
  public long run(int fromIndex, int toIndex) {
    int fromNode = indexManager.indexToNode(fromIndex);
    int toNode = indexManager.indexToNode(toIndex);
    return distances[fromNode][toNode];
  }
}

class Vrp {
  static {
    System.loadLibrary("jniortools");
  }

  /// @brief Add Global Span constraint.
  static void addDistanceDimension(RoutingModel routing, DataProblem data, int distanceIndex) {
    String distance = "Distance";
    routing.addDimension(
        distanceIndex,
        0, // null slack
        3000, // maximum distance per vehicle
        true, // start cumul to zero
        distance);
    RoutingDimension distanceDimension = routing.getDimensionOrDie(distance);
    // Try to minimize the max distance among vehicles.
    // /!\ It doesn't mean the standard deviation is minimized
    distanceDimension.setGlobalSpanCostCoefficient(100);
  }

  /// @brief Print the solution
  static void printSolution(
      DataProblem data, RoutingModel routing, RoutingIndexManager manager, Assignment solution) {
    // Solution cost.
    System.out.println("Objective : " + solution.objectiveValue());
    // Inspect solution.
    for (int i = 0; i < data.getVehicleNumber(); ++i) {
      System.out.println("Route for Vehicle " + i + ":");
      long distance = 0;
      for (long index = routing.start(i); !routing.isEnd(index); ) {
        System.out.print(manager.indexToNode((int) index) + " -> ");

        long previousIndex = index;
        index = solution.value(routing.nextVar(index));
        distance += routing.getArcCostForVehicle(previousIndex, index, i);
      }
      System.out.println(manager.indexToNode((int) routing.end(i)));
      System.out.println("Distance of the route: " + distance + "m");
    }
  }

  /// @brief Solves the current routing problem.
  static void solve() {
    // Instantiate the data problem.
    DataProblem data = new DataProblem();

    // Create Routing Model
    RoutingIndexManager manager =
        new RoutingIndexManager(data.getLocationNumber(), data.getVehicleNumber(), data.getDepot());
    RoutingModel routing = new RoutingModel(manager);

    // Setting the cost function.
    // [todo]: protect callback from the GC
    IntIntToLong distanceEvaluator = new ManhattanDistance(data, manager);
    int distanceIndex = routing.registerTransitCallback(distanceEvaluator);
    routing.setArcCostEvaluatorOfAllVehicles(distanceIndex);
    addDistanceDimension(routing, data, distanceIndex);

    // Setting first solution heuristic (cheapest addition).
    RoutingSearchParameters search_parameters =
        RoutingSearchParameters.newBuilder()
            .mergeFrom(main.defaultRoutingSearchParameters())
            .setFirstSolutionStrategy(FirstSolutionStrategy.Value.PATH_CHEAPEST_ARC)
            .build();

    Assignment solution = routing.solveWithParameters(search_parameters);
    printSolution(data, routing, manager, solution);
  }

  /// @brief Entry point of the program.
  public static void main(String[] args) throws Exception {
    solve();
  }
}
